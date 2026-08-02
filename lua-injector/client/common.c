/*
 * common.c — shared client core (see common.h).
 *
 * Injection methods implemented here:
 *
 *  1. LOADLIBRARY — the classic: resolve LoadLibraryA in the target
 *     (same bitness: reuse our kernel32's address; 64->32: WOW64 PEB walk
 *     + export walk), write the DLL path there, CreateRemoteThread.
 *
 *  2. MANUAL — read the DLL image from disk, allocate space in the target,
 *     copy headers + sections, fix up relocations, resolve every import
 *     against the modules already loaded in the target (toolhelp snapshot
 *     + in-target export walk), flip the host's lua_host_manual_map flag,
 *     and CreateRemoteThread at AddressOfEntryPoint.  No LoadLibrary, no
 *     loader involvement.
 *
 *  AUTO tries 1 and falls back to 2.
 */
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <tlhelp32.h>

#define CC_PIPE_PREFIX   "\\\\.\\pipe\\lua_host_%lu"
#define CC_MAX_REPLY     (16u << 20)
#define CC_INJECT_TIMEOUT_MS 15000

/* ================================================================== */
/* small helpers                                                     */
/* ================================================================== */

static int rpm(HANDLE h, uintptr_t addr, void *dst, size_t n) {
    SIZE_T rd = 0;
    return ReadProcessMemory(h, (LPCVOID)addr, dst, n, &rd) != 0 && rd == n;
}

static int wpm(HANDLE h, uintptr_t addr, const void *src, size_t n) {
    SIZE_T wr = 0;
    return WriteProcessMemory(h, (LPVOID)addr, src, n, &wr) != 0 && wr == n;
}

/* ================================================================== */
/* process enumeration                                               */
/* ================================================================== */

static int proc_sort(const void *a, const void *b) {
    DWORD pa = ((const cc_proc *)a)->pid;
    DWORD pb = ((const cc_proc *)b)->pid;
    return (pa > pb) - (pa < pb);
}

int cc_list_processes(cc_proc **out, int *count) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return -1;
    int cap = 512, n = 0;
    cc_proc *arr = (cc_proc *)malloc((size_t)cap * sizeof(cc_proc));
    if (!arr) { CloseHandle(snap); return -1; }
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (n == cap) {
                cap *= 2;
                cc_proc *na = (cc_proc *)realloc(arr,
                                    (size_t)cap * sizeof(cc_proc));
                if (!na) break;
                arr = na;
            }
            arr[n].bits = cc_process_bits(pe.th32ProcessID);
            arr[n].pid  = pe.th32ProcessID;
            arr[n].ppid = pe.th32ParentProcessID;
            wcsncpy(arr[n].name, pe.szExeFile, 255);
            arr[n].name[255] = L'\0';
            n++;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    qsort(arr, (size_t)n, sizeof(cc_proc), proc_sort);
    *out = arr;
    *count = n;
    return 0;
}

void cc_free_processes(cc_proc *arr) { free(arr); }

int cc_process_bits(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION |
                           PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) return 0;
    BOOL wow = FALSE;
    if (!IsWow64Process(h, &wow)) {
        CloseHandle(h);
        return (int)(sizeof(void *) * 8);
    }
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    CloseHandle(h);
    if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ||
        si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64)
        return wow ? 32 : 64;
    return 32;
}

const char *cc_method_name(int method) {
    switch (method) {
    case CC_METHOD_LOADLIBRARY: return "LoadLibrary remote-thread";
    case CC_METHOD_MANUAL:      return "manual map";
    default:                    return "auto (loadlibrary, then manual)";
    }
}

/* ================================================================== */
/* export resolution inside the target (works for 32 and 64 bit)      */
/* ================================================================== */

typedef LONG(WINAPI *nt_query_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);
#define PROCESS_WOW64_INFORMATION 26

/* resolve an exported function of the module at `base` in `hProc`.
 * if name != NULL lookup by name, else by ordinal. */
static uintptr_t find_export(HANDLE hProc, uintptr_t base,
                             const char *name, WORD ordinal) {
    IMAGE_DOS_HEADER dos;
    if (!rpm(hProc, base, &dos, sizeof dos)) return 0;
    if (dos.e_magic != IMAGE_DOS_SIGNATURE) return 0;

    /* detect PE32 vs PE32+ to locate the data directory */
    WORD magic = 0;
    if (!rpm(hProc, base + dos.e_lfanew + 24, &magic, 2)) return 0;
    DWORD ddRVA, ddSize;
    if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        /* IMAGE_NT_HEADERS32: opt starts +24, DataDirectory at opt+96 */
        IMAGE_NT_HEADERS32 nth;
        if (!rpm(hProc, base + dos.e_lfanew, &nth, sizeof nth)) return 0;
        if (nth.Signature != IMAGE_NT_SIGNATURE) return 0;
        ddRVA = nth.OptionalHeader.DataDirectory[0].VirtualAddress;
        ddSize = nth.OptionalHeader.DataDirectory[0].Size;
    } else if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        IMAGE_NT_HEADERS64 nth;
        if (!rpm(hProc, base + dos.e_lfanew, &nth, sizeof nth)) return 0;
        if (nth.Signature != IMAGE_NT_SIGNATURE) return 0;
        ddRVA = nth.OptionalHeader.DataDirectory[0].VirtualAddress;
        ddSize = nth.OptionalHeader.DataDirectory[0].Size;
    } else {
        return 0;
    }
    if (!ddRVA || !ddSize) return 0;

    IMAGE_EXPORT_DIRECTORY exp;
    if (!rpm(hProc, base + ddRVA, &exp, sizeof exp)) return 0;
    if (exp.NumberOfNames == 0 || exp.NumberOfFunctions == 0) return 0;

    if (name) {
        DWORD *names = (DWORD *)malloc(exp.NumberOfNames * sizeof(DWORD));
        WORD  *ords  = (WORD  *)malloc(exp.NumberOfNames * sizeof(WORD));
        DWORD *funcs = (DWORD *)malloc(exp.NumberOfFunctions * sizeof(DWORD));
        if (!names || !ords || !funcs) {
            free(names); free(ords); free(funcs);
            return 0;
        }
        int ok = rpm(hProc, base + exp.AddressOfNames, names,
                     exp.NumberOfNames * sizeof(DWORD)) &&
                 rpm(hProc, base + exp.AddressOfNameOrdinals, ords,
                     exp.NumberOfNames * sizeof(WORD)) &&
                 rpm(hProc, base + exp.AddressOfFunctions, funcs,
                     exp.NumberOfFunctions * sizeof(DWORD));
        uintptr_t found = 0;
        if (ok) {
            for (DWORD i = 0; i < exp.NumberOfNames && !found; i++) {
                char nm[128] = {0};
                if (!rpm(hProc, base + names[i], nm, sizeof nm - 1)) continue;
                if (strcmp(nm, name) == 0) {
                    WORD o = ords[i];
                    if (o < exp.NumberOfFunctions) found = base + funcs[o];
                }
            }
        }
        free(names); free(ords); free(funcs);
        return found;
    } else {
        DWORD *funcs = (DWORD *)malloc(exp.NumberOfFunctions * sizeof(DWORD));
        if (!funcs) return 0;
        int ok = rpm(hProc, base + exp.AddressOfFunctions, funcs,
                     exp.NumberOfFunctions * sizeof(DWORD));
        uintptr_t found = 0;
        if (ok) {
            DWORD idx = (DWORD)ordinal - exp.Base;
            if (idx < exp.NumberOfFunctions) found = base + funcs[idx];
        }
        free(funcs);
        return found;
    }
}

/* 64-bit injector -> 32-bit target: LoadLibraryA from the WOW64 PEB */
static uintptr_t loadlibrary_peb_walk(HANDLE hProc) {
    nt_query_t ntq = (nt_query_t)GetProcAddress(GetModuleHandleA("ntdll.dll"),
                                                "NtQueryInformationProcess");
    if (!ntq) return 0;
    ULONG_PTR peb32 = 0;
    ULONG len = 0;
    if (ntq(hProc, PROCESS_WOW64_INFORMATION, &peb32, sizeof peb32, &len) < 0)
        return 0;
    if (!peb32) return 0;

    ULONG ldr = 0;                       /* PEB32+0x0C */
    if (!rpm(hProc, peb32 + 0x0C, &ldr, 4) || !ldr) return 0;
    ULONG head = 0;                      /* PEB_LDR_DATA32+0x0C */
    if (!rpm(hProc, ldr + 0x0C, &head, 4) || !head) return 0;

    ULONG cur = head;
    for (int guard = 0; guard < 1024; guard++) {
        ULONG base = 0;
        USHORT nlen = 0;
        ULONG nbuff = 0;
        /* LDR_DATA_TABLE_ENTRY32: Flink+0x00, DllBase+0x18,
         * FullDllName.Length+0x24, FullDllName.Buffer+0x2C */
        if (!rpm(hProc, cur + 0x18, &base, 4)) return 0;
        if (!rpm(hProc, cur + 0x24, &nlen, 2)) return 0;
        if (!rpm(hProc, cur + 0x2C, &nbuff, 4)) return 0;
        if (nlen && nbuff) {
            wchar_t nm[256] = {0};
            int cc = nlen / 2;
            if (cc > 255) cc = 255;
            if (rpm(hProc, nbuff, nm, (size_t)cc * 2) &&
                lstrcmpiW(nm, L"kernel32.dll") == 0)
                return find_export(hProc, base, "LoadLibraryA", 0);
        }
        ULONG next = 0;
        if (!rpm(hProc, cur, &next, 4)) return 0;
        if (next == head || next == 0) break;
        cur = next;
    }
    return 0;
}

static uintptr_t resolve_loadlibrary(HANDLE hProc, int target64, int self64) {
    if (self64 == target64) {
        return (uintptr_t)GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                         "LoadLibraryA");
    }
    /* only valid direction left: 64 -> 32 */
    return loadlibrary_peb_walk(hProc);
}

/* ================================================================== */
/* method 1: classic LoadLibrary remote-thread                        */
/* ================================================================== */

static int inject_loadlibrary(HANDLE hProc, const char *dll_path,
                              int target64, uintptr_t *base_out,
                              char *err, size_t errlen) {
    int self64 = (int)(sizeof(void *) * 8);
    if (target64 == 64 && self64 == 32) {
        snprintf(err, errlen, "32-bit injector cannot inject into a "
                 "64-bit process. use lua-injector-x64.exe instead.");
        return -1;
    }
    uintptr_t loadlib = resolve_loadlibrary(hProc, target64, self64);
    if (!loadlib) {
        snprintf(err, errlen, "could not resolve LoadLibraryA in the "
                 "target process (arch mismatch or protected process?)");
        return -1;
    }

    size_t plen = strlen(dll_path) + 1;
    LPVOID remote = VirtualAllocEx(hProc, NULL, plen,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        snprintf(err, errlen, "VirtualAllocEx failed (%lu)",
                 (unsigned long)GetLastError());
        return -1;
    }
    if (!wpm(hProc, (uintptr_t)remote, dll_path, plen)) {
        snprintf(err, errlen, "WriteProcessMemory failed (%lu)",
                 (unsigned long)GetLastError());
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        return -1;
    }
    HANDLE th = CreateRemoteThread(hProc, NULL, 0,
                                   (LPTHREAD_START_ROUTINE)loadlib,
                                   remote, 0, NULL);
    if (!th) {
        snprintf(err, errlen, "CreateRemoteThread failed (%lu)",
                 (unsigned long)GetLastError());
        VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
        return -1;
    }
    DWORD wait = WaitForSingleObject(th, CC_INJECT_TIMEOUT_MS);
    DWORD exitc = 0;
    GetExitCodeThread(th, &exitc);
    CloseHandle(th);
    VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);

    if (wait == WAIT_TIMEOUT) {
        snprintf(err, errlen, "LoadLibraryA inside the target timed out");
        return -1;
    }
    if (exitc == 0) {
        snprintf(err, errlen, "LoadLibraryA returned NULL — the DLL may "
                 "be the wrong bitness or the path is wrong");
        return -1;
    }
    if (base_out) *base_out = (uintptr_t)exitc;
    return 0;
}

/* ================================================================== */
/* method 2: manual map                                               */
/* ================================================================== */

/* in-memory PE view of the local DLL file */
typedef struct pe_file {
    BYTE   *img;
    size_t  len;
    IMAGE_DOS_HEADER *dos;
    WORD    magic;                  /* 0x10b / 0x20b */
    DWORD   image_base;
    DWORD   size_of_image;
    DWORD   size_of_headers;
    DWORD   entry_rva;
    WORD    n_sections;
    IMAGE_SECTION_HEADER *secs;
} pe_file_t;

static int pe_open(const char *path, pe_file_t *pe) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > (64 << 20)) { fclose(f); return -1; }
    BYTE *buf = (BYTE *)malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);

    memset(pe, 0, sizeof *pe);
    pe->img = buf;
    pe->len = (size_t)sz;
    if (sz < (long)sizeof(IMAGE_DOS_HEADER)) { free(buf); return -1; }
    pe->dos = (IMAGE_DOS_HEADER *)buf;
    if (pe->dos->e_magic != IMAGE_DOS_SIGNATURE) { free(buf); return -1; }

    DWORD ntoff = pe->dos->e_lfanew;
    if (ntoff + 4 + 20 > (DWORD)sz) { free(buf); return -1; }
    DWORD sig = *(DWORD *)(buf + ntoff);
    if (sig != IMAGE_NT_SIGNATURE) { free(buf); return -1; }

    WORD magic = *(WORD *)(buf + ntoff + 24);
    if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        if (ntoff + sizeof(IMAGE_NT_HEADERS32) > (DWORD)sz) { free(buf); return -1; }
        IMAGE_NT_HEADERS32 *nth = (IMAGE_NT_HEADERS32 *)(buf + ntoff);
        pe->magic          = magic;
        pe->image_base     = nth->OptionalHeader.ImageBase;
        pe->size_of_image  = nth->OptionalHeader.SizeOfImage;
        pe->size_of_headers= nth->OptionalHeader.SizeOfHeaders;
        pe->entry_rva      = nth->OptionalHeader.AddressOfEntryPoint;
        pe->n_sections     = nth->FileHeader.NumberOfSections;
        pe->secs = (IMAGE_SECTION_HEADER *)(buf + ntoff +
                    24 + sizeof(IMAGE_OPTIONAL_HEADER32));
    } else if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        if (ntoff + sizeof(IMAGE_NT_HEADERS64) > (DWORD)sz) { free(buf); return -1; }
        IMAGE_NT_HEADERS64 *nth = (IMAGE_NT_HEADERS64 *)(buf + ntoff);
        pe->magic          = magic;
        pe->image_base     = (DWORD)nth->OptionalHeader.ImageBase;
        pe->size_of_image  = nth->OptionalHeader.SizeOfImage;
        pe->size_of_headers= nth->OptionalHeader.SizeOfHeaders;
        pe->entry_rva      = nth->OptionalHeader.AddressOfEntryPoint;
        pe->n_sections     = nth->FileHeader.NumberOfSections;
        pe->secs = (IMAGE_SECTION_HEADER *)(buf + ntoff +
                    24 + sizeof(IMAGE_OPTIONAL_HEADER64));
    } else {
        free(buf);
        return -1;
    }
    if (pe->n_sections > 96 || pe->size_of_image == 0 ||
        pe->size_of_image > (256u << 20)) {
        free(buf);
        return -1;
    }
    return 0;
}

static void pe_close(pe_file_t *pe) { free(pe->img); pe->img = NULL; }

/* convert an RVA in the local image to a file offset (0 if invalid) */
static DWORD pe_rva_to_off(const pe_file_t *pe, DWORD rva) {
    if (rva < pe->size_of_headers) return rva;
    for (WORD i = 0; i < pe->n_sections; i++) {
        IMAGE_SECTION_HEADER *s = &pe->secs[i];
        DWORD end = s->VirtualAddress + s->SizeOfRawData;
        if (rva >= s->VirtualAddress && rva < end &&
            s->PointerToRawData + (rva - s->VirtualAddress) < pe->len)
            return s->PointerToRawData + (rva - s->VirtualAddress);
    }
    return 0;
}

/* find the local module base of `dllname` in the target process */
static uintptr_t module_in_target(DWORD pid, const wchar_t *dllname) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE |
                                           TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32W me;
    me.dwSize = sizeof(me);
    uintptr_t found = 0;
    if (Module32FirstW(snap, &me)) {
        do {
            if (lstrcmpiW(me.szModule, dllname) == 0) {
                found = (uintptr_t)me.modBaseAddr;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

static int manual_map(HANDLE hProc, DWORD pid, const char *dll_path,
                      uintptr_t *base_out, char *err, size_t errlen) {
    pe_file_t pe;
    if (pe_open(dll_path, &pe) != 0) {
        snprintf(err, errlen, "cannot parse DLL image: %s", dll_path);
        return -1;
    }

    /* 1. allocate space in the target */
    LPVOID remote = VirtualAllocEx(hProc, NULL, pe.size_of_image,
                                   MEM_COMMIT | MEM_RESERVE,
                                   PAGE_EXECUTE_READWRITE);
    if (!remote) {
        snprintf(err, errlen, "VirtualAllocEx (%lu bytes) failed (%lu)",
                 (unsigned long)pe.size_of_image,
                 (unsigned long)GetLastError());
        pe_close(&pe);
        return -1;
    }
    uintptr_t rb = (uintptr_t)remote;

    /* 2. headers + sections */
    if (!wpm(hProc, rb, pe.img, pe.size_of_headers)) {
        snprintf(err, errlen, "could not write headers (%lu)",
                 (unsigned long)GetLastError());
        goto fail;
    }
    for (WORD i = 0; i < pe.n_sections; i++) {
        IMAGE_SECTION_HEADER *s = &pe.secs[i];
        if (s->VirtualAddress == 0) continue;
        DWORD copy = s->SizeOfRawData;
        if (s->PointerToRawData + copy > pe.len) copy = pe.len - s->PointerToRawData;
        if (copy > s->Misc.VirtualSize) copy = s->Misc.VirtualSize;
        uintptr_t dst = rb + s->VirtualAddress;
        if (copy)
            if (!wpm(hProc, dst, pe.img + s->PointerToRawData, copy)) {
                snprintf(err, errlen, "could not write section %u (%lu)",
                         i, (unsigned long)GetLastError());
                goto fail;
            }
        /* zero-fill BSS (VirtualSize > SizeOfRawData) */
        if (s->Misc.VirtualSize > copy) {
            SIZE_T z = s->Misc.VirtualSize - copy;
            BYTE *zero = (BYTE *)calloc(1, 4096);
            while (z) {
                SIZE_T c = z > 4096 ? 4096 : z;
                if (!zero || !wpm(hProc, dst + copy, zero, c)) break;
                z -= c;
                copy += c;
            }
            free(zero);
        }
    }

    /* 3. relocations */
    int64_t delta = (int64_t)(rb - (uintptr_t)pe.image_base);
    if (delta != 0) {
        DWORD dir_off = 0;
        /* data directory index 5 = base reloc */
        DWORD ntoff = pe.dos->e_lfanew;
        if (pe.magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
            IMAGE_NT_HEADERS32 *nth = (IMAGE_NT_HEADERS32 *)(pe.img + ntoff);
            dir_off = pe_rva_to_off(&pe,
                     nth->OptionalHeader.DataDirectory[5].VirtualAddress);
        } else {
            IMAGE_NT_HEADERS64 *nth = (IMAGE_NT_HEADERS64 *)(pe.img + ntoff);
            dir_off = pe_rva_to_off(&pe,
                     nth->OptionalHeader.DataDirectory[5].VirtualAddress);
        }
        while (dir_off && dir_off + sizeof(IMAGE_BASE_RELOCATION) <= pe.len) {
            IMAGE_BASE_RELOCATION blk;
            memcpy(&blk, pe.img + dir_off, sizeof blk);
            if (blk.VirtualAddress == 0 && blk.SizeOfBlock == 0) break;
            DWORD count = (blk.SizeOfBlock - sizeof blk) / 2;
            for (DWORD i = 0; i < count; i++) {
                WORD e = *(WORD *)(pe.img + dir_off + sizeof blk + i * 2);
                int type = e >> 12;
                DWORD off = e & 0xFFF;
                if (type == 0) continue;               /* ABSOLUTE */
                uintptr_t a = rb + blk.VirtualAddress + off;
                if (type == 3) {                       /* HIGHLOW (PE32) */
                    DWORD v;
                    if (!rpm(hProc, a, &v, 4)) continue;
                    v = (DWORD)(v + (DWORD)delta);
                    wpm(hProc, a, &v, 4);
                } else if (type == 10) {               /* DIR64 (PE32+) */
                    uint64_t v;
                    if (!rpm(hProc, a, &v, 8)) continue;
                    v = (uint64_t)(v + delta);
                    wpm(hProc, a, &v, 8);
                }
            }
            dir_off += blk.SizeOfBlock;
        }
    }

    /* 4. imports */
    {
        DWORD ntoff = pe.dos->e_lfanew;
        DWORD imp_rva = 0;
        if (pe.magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
            IMAGE_NT_HEADERS32 *nth = (IMAGE_NT_HEADERS32 *)(pe.img + ntoff);
            imp_rva = nth->OptionalHeader.DataDirectory[1].VirtualAddress;
        } else {
            IMAGE_NT_HEADERS64 *nth = (IMAGE_NT_HEADERS64 *)(pe.img + ntoff);
            imp_rva = nth->OptionalHeader.DataDirectory[1].VirtualAddress;
        }
        if (imp_rva) {
            DWORD off = pe_rva_to_off(&pe, imp_rva);
            int broken = 0;
            while (off && off + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= pe.len) {
                IMAGE_IMPORT_DESCRIPTOR id;
                memcpy(&id, pe.img + off, sizeof id);
                if (id.Name == 0) break;
                char dllname[128] = {0};
                DWORD noff = pe_rva_to_off(&pe, id.Name);
                if (!noff || noff >= pe.len) { off += sizeof id; continue; }
                strncpy(dllname, (char *)pe.img + noff, 127);

                wchar_t wname[128];
                if (MultiByteToWideChar(CP_UTF8, 0, dllname, -1,
                                        wname, 128) == 0)
                    wname[0] = L'\0';
                uintptr_t mod = module_in_target(pid, wname);
                if (!mod) {
                    /* try with ".dll" appended / stripped */
                    wchar_t w2[160];
                    if (wcschr(wname, L'.'))
                        swprintf(w2, 160, L"%s", wname);
                    else
                        swprintf(w2, 160, L"%s.dll", wname);
                    mod = module_in_target(pid, w2);
                }
                if (!mod) {
                    snprintf(err, errlen,
                             "manual map: import '%s' is not loaded in the "
                             "target", dllname);
                    broken = 1;
                    break;
                }

                DWORD thunk_rva = id.FirstThunk;
                DWORD thunk_off = pe_rva_to_off(&pe, thunk_rva);
                DWORD oft_rva = id.OriginalFirstThunk ? id.OriginalFirstThunk
                                                      : id.FirstThunk;
                DWORD oft_off = pe_rva_to_off(&pe, oft_rva);
                int is64 = (pe.magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC);
                DWORD i = 0;
                for (;; i++) {
                    uint64_t thunk = 0;
                    if (is64) {
                        if (oft_off + (i + 1) * 8 > pe.len) break;
                        thunk = *(uint64_t *)(pe.img + oft_off + i * 8);
                    } else {
                        if (oft_off + (i + 1) * 4 > pe.len) break;
                        thunk = *(DWORD *)(pe.img + oft_off + i * 4);
                    }
                    if (thunk == 0) break;
                    uintptr_t fn = 0;
                    if (is64 ? (thunk & 0x8000000000000000ULL)
                             : (thunk & 0x80000000UL)) {
                        fn = find_export(hProc, mod, NULL,
                                         (WORD)(thunk & 0xFFFF));
                    } else {
                        DWORD iname_off = pe_rva_to_off(&pe, (DWORD)thunk);
                        char fnname[256] = {0};
                        if (iname_off && iname_off + 2 < pe.len)
                            strncpy(fnname, (char *)pe.img + iname_off + 2, 255);
                        fn = find_export(hProc, mod, fnname, 0);
                    }
                    if (!fn) {
                        snprintf(err, errlen,
                                 "manual map: could not resolve an import "
                                 "from '%s'", dllname);
                        broken = 1;
                        break;
                    }
                    if (is64)
                        wpm(hProc, rb + thunk_rva + i * 8, &fn, 8);
                    else {
                        DWORD f32 = (DWORD)fn;
                        wpm(hProc, rb + thunk_rva + i * 4, &f32, 4);
                    }
                }
                if (broken) break;
                off += sizeof id;
            }
            if (broken) goto fail;
        }
    }

    /* 5. tell the host it was manually mapped (so it skips FreeLibrary) */
    {
        DWORD ntoff = pe.dos->e_lfanew;
        DWORD exp_rva = 0;
        if (pe.magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
            IMAGE_NT_HEADERS32 *nth = (IMAGE_NT_HEADERS32 *)(pe.img + ntoff);
            exp_rva = nth->OptionalHeader.DataDirectory[0].VirtualAddress;
        } else {
            IMAGE_NT_HEADERS64 *nth = (IMAGE_NT_HEADERS64 *)(pe.img + ntoff);
            exp_rva = nth->OptionalHeader.DataDirectory[0].VirtualAddress;
        }
        if (exp_rva) {
            DWORD eoff = pe_rva_to_off(&pe, exp_rva);
            if (eoff && eoff + sizeof(IMAGE_EXPORT_DIRECTORY) <= pe.len) {
                IMAGE_EXPORT_DIRECTORY exp;
                memcpy(&exp, pe.img + eoff, sizeof exp);
                DWORD names_rva = exp.AddressOfNames;
                DWORD ords_rva  = exp.AddressOfNameOrdinals;
                DWORD funcs_rva = exp.AddressOfFunctions;
                for (DWORD i = 0; i < exp.NumberOfNames; i++) {
                    DWORD nr = pe_rva_to_off(&pe, names_rva + i * 4);
                    char nm[64] = {0};
                    if (!nr || nr >= pe.len) continue;
                    strncpy(nm, (char *)pe.img + nr, 63);
                    if (strcmp(nm, "lua_host_manual_map") == 0) {
                        DWORD oo = pe_rva_to_off(&pe, ords_rva + i * 2);
                        DWORD fo = pe_rva_to_off(&pe, funcs_rva);
                        if (oo && fo && oo + 2 <= pe.len) {
                            WORD ord = *(WORD *)(pe.img + oo);
                            DWORD fn_rva = *(DWORD *)(pe.img + fo + ord * 4);
                            uintptr_t dst = rb + fn_rva;
                            int one = 1;
                            wpm(hProc, dst, &one, sizeof one);
                        }
                        break;
                    }
                }
            }
        }
    }

    /* 6. run the entry point */
    if (pe.entry_rva) {
        HANDLE th = CreateRemoteThread(hProc, NULL, 0,
                                       (LPTHREAD_START_ROUTINE)(rb + pe.entry_rva),
                                       (LPVOID)rb, 0, NULL);
        if (!th) {
            snprintf(err, errlen, "manual map: CreateRemoteThread failed "
                     "(%lu)", (unsigned long)GetLastError());
            goto fail;
        }
        DWORD wait = WaitForSingleObject(th, CC_INJECT_TIMEOUT_MS);
        CloseHandle(th);
        if (wait == WAIT_TIMEOUT) {
            snprintf(err, errlen, "manual map: entry point timed out");
            goto fail;
        }
    }

    if (base_out) *base_out = rb;
    pe_close(&pe);
    return 0;

fail:
    VirtualFreeEx(hProc, remote, 0, MEM_RELEASE);
    pe_close(&pe);
    return -1;
}

/* ================================================================== */
/* cc_inject                                                          */
/* ================================================================== */

int cc_inject(DWORD pid, const char *dll, int method,
              uintptr_t *base_out, int *method_used,
              char *err, size_t errlen) {
    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION |
                               PROCESS_QUERY_LIMITED_INFORMATION |
                               PROCESS_VM_OPERATION |
                               PROCESS_VM_WRITE |
                               PROCESS_VM_READ |
                               PROCESS_CREATE_THREAD,
                               FALSE, pid);
    if (!hProc) {
        snprintf(err, errlen,
                 "cannot open process %lu (%lu). it may not exist, or it "
                 "runs with higher privileges than this console — run as "
                 "administrator.",
                 (unsigned long)pid, (unsigned long)GetLastError());
        return -1;
    }
    int tbits = cc_process_bits(pid);
    if (!tbits) {
        snprintf(err, errlen, "could not determine the bitness of pid %lu",
                 (unsigned long)pid);
        CloseHandle(hProc);
        return -1;
    }
    if (tbits == 64 && (int)(sizeof(void *) * 8) == 32) {
        snprintf(err, errlen, "32-bit injector cannot inject into a "
                 "64-bit process. use lua-injector-x64.exe instead.");
        CloseHandle(hProc);
        return -1;
    }

    if (method != CC_METHOD_MANUAL) {
        int rc = inject_loadlibrary(hProc, dll, tbits, base_out, err, errlen);
        if (rc == 0) {
            if (method_used) *method_used = CC_METHOD_LOADLIBRARY;
            CloseHandle(hProc);
            return 0;
        }
        if (method == CC_METHOD_LOADLIBRARY) {
            CloseHandle(hProc);
            return -1;
        }
        /* AUTO: fall through to manual */
    }
    int rc = manual_map(hProc, pid, dll, base_out, err, errlen);
    if (rc == 0 && method_used) *method_used = CC_METHOD_MANUAL;
    CloseHandle(hProc);
    return rc;
}

/* ================================================================== */
/* pipe client                                                        */
/* ================================================================== */

HANDLE cc_connect(DWORD pid, int tries) {
    char name[64];
    snprintf(name, sizeof name, CC_PIPE_PREFIX, (unsigned long)pid);
    for (int i = 0; i < tries; i++) {
        HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                               OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) return h;
        if (GetLastError() == ERROR_PIPE_BUSY) {
            if (!WaitNamedPipeA(name, 200)) { Sleep(100); continue; }
            continue;
        }
        Sleep(100);
    }
    return INVALID_HANDLE_VALUE;
}

static int pipe_write_all(HANDLE pipe, const void *src, size_t n) {
    const unsigned char *p = (const unsigned char *)src;
    while (n) {
        DWORD wr = 0;
        if (!WriteFile(pipe, p, (DWORD)(n > 0x7FFFFFFF ? 0x7FFFFFFF : n),
                       &wr, NULL) || wr == 0)
            return -1;
        p += wr;
        n -= wr;
    }
    return 0;
}

static size_t pipe_read_line(HANDLE pipe, char *dst, size_t max) {
    size_t n = 0;
    char c;
    while (n + 1 < max) {
        DWORD rd = 0;
        if (!ReadFile(pipe, &c, 1, &rd, NULL) || rd == 0) break;
        if (c == '\n') break;
        dst[n++] = c;
    }
    if (n && dst[n - 1] == '\r') n--;
    dst[n] = '\0';
    return n;
}

static int pipe_read_exact(HANDLE pipe, void *dst, size_t n) {
    unsigned char *p = (unsigned char *)dst;
    while (n) {
        DWORD rd = 0;
        if (!ReadFile(pipe, p, (DWORD)(n > 0x7FFFFFFF ? 0x7FFFFFFF : n),
                      &rd, NULL) || rd == 0)
            return -1;
        p += rd;
        n -= rd;
    }
    return 0;
}

int cc_command(HANDLE pipe, const char *cmd, const void *data, size_t n,
               char **out, size_t *olen) {
    char hdr[64];
    int hlen = snprintf(hdr, sizeof hdr, "%s\n%zu\n", cmd, n);
    if (hlen <= 0) return -1;
    if (pipe_write_all(pipe, hdr, (size_t)hlen) != 0) return -1;
    if (n && pipe_write_all(pipe, data, n) != 0) return -1;

    char status[4] = {0};
    char lenbuf[32] = {0};
    if (pipe_read_line(pipe, status, sizeof status) == 0) return -1;
    if (pipe_read_line(pipe, lenbuf, sizeof lenbuf) == 0) return -1;
    unsigned long rn = strtoul(lenbuf, NULL, 10);
    if (rn > CC_MAX_REPLY) rn = CC_MAX_REPLY;
    char *buf = (char *)malloc((size_t)rn + 1);
    if (!buf) return -1;
    if (rn && pipe_read_exact(pipe, buf, (size_t)rn) != 0) {
        free(buf);
        return -1;
    }
    buf[rn] = '\0';
    *out = buf;
    *olen = (size_t)rn;
    return strcmp(status, "OK") == 0 ? 0 : 1;
}

void cc_free(char *p) { free(p); }
