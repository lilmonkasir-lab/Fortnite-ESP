import threading
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

from lua_injection.client import AttachError, LuaInjectionClient
from lua_injection.host import HostDescriptor, HostService
from lua_injection.protocol import read_message, write_message


class HostClientTests(unittest.TestCase):
    def setUp(self):
        self.service = HostService(name="test-host", initial_state={"counter": 4})
        self.thread = threading.Thread(target=self.service.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.service.shutdown()
        self.thread.join(timeout=2)

    def test_descriptor_round_trip(self):
        with TemporaryDirectory() as directory:
            path = Path(directory) / "host.json"
            self.service.descriptor.write(path)
            loaded = HostDescriptor.read(path)
        self.assertEqual(loaded.to_dict(), self.service.descriptor.to_dict())

    def test_attach_execute_and_state(self):
        client = LuaInjectionClient(self.service.descriptor)
        try:
            hello = client.connect()
            self.assertEqual(hello["name"], "test-host")
            result = client.execute(
                "local n = host.get('counter')\n"
                "host.set('counter', n + 1)\n"
                "host.emit('incremented', { value = n + 1 })\n"
                "return host.get('counter')"
            )
            self.assertEqual(result["status"], "ok")
            self.assertEqual(result["result"], 5)
            self.assertEqual(result["state"]["counter"], 5)
            self.assertEqual(result["events"][0]["name"], "incremented")
        finally:
            client.detach()

    def test_bad_token_is_rejected(self):
        descriptor = HostDescriptor(
            host=self.service.descriptor.host,
            port=self.service.descriptor.port,
            host_id=self.service.descriptor.host_id,
            name=self.service.descriptor.name,
            token="wrong-token",
        )
        client = LuaInjectionClient(descriptor)
        with self.assertRaises(AttachError):
            client.connect()

    def test_validate_does_not_mutate_state(self):
        client = LuaInjectionClient(self.service.descriptor)
        try:
            result = client.validate("host.set('counter', 999)")
            self.assertEqual(result["status"], "ok")
            self.assertTrue(result["validated"])
            self.assertEqual(client.get_state()["state"]["counter"], 4)
        finally:
            client.detach()


if __name__ == "__main__":
    unittest.main()
