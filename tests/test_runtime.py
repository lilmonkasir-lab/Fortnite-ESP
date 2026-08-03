import unittest

from lua_injection.runtime import HostAPI, Sandbox


class SandboxTests(unittest.TestCase):
    def test_arithmetic_and_return(self):
        result = Sandbox().execute("local x = 2 + 3 * 4\nreturn x")
        self.assertEqual(result["status"], "ok")
        self.assertEqual(result["result"], 14)

    def test_functions_tables_and_loops(self):
        source = """
        local function sum(values)
          local total = 0
          for _, value in ipairs(values) do
            total = total + value
          end
          return total
        end
        return sum({ 1, 2, 3, 4 })
        """
        result = Sandbox().execute(source)
        self.assertEqual(result["status"], "ok")
        self.assertEqual(result["result"], 10)

    def test_host_state_and_events(self):
        host = HostAPI(name="fixture", state={"counter": 2})
        result = Sandbox(host).execute(
            "local n = host.get('counter')\n"
            "host.set('counter', n + 1)\n"
            "host.emit('changed', { value = n + 1 })\n"
            "return host.snapshot()"
        )
        self.assertEqual(result["status"], "ok")
        self.assertEqual(result["result"], {"counter": 3})
        self.assertEqual(result["events"][0]["name"], "changed")
        self.assertEqual(result["events"][0]["payload"], {"value": 3})

    def test_instruction_limit_stops_infinite_loop(self):
        result = Sandbox(max_instructions=50).execute("local x = 0\nwhile true do x = x + 1 end")
        self.assertEqual(result["status"], "error")
        self.assertIn("instruction limit", result["error"])

    def test_unsafe_library_is_not_available(self):
        result = Sandbox().execute("return os.execute('echo no')")
        self.assertEqual(result["status"], "error")
        self.assertIn("cannot index a nil", result["error"])

    def test_events_do_not_repeat_between_executions(self):
        host = HostAPI()
        sandbox = Sandbox(host)
        first = sandbox.execute("host.emit('first')")
        second = sandbox.execute("host.emit('second')")
        self.assertEqual([event["name"] for event in first["events"]], ["first"])
        self.assertEqual([event["name"] for event in second["events"]], ["second"])


if __name__ == "__main__":
    unittest.main()
