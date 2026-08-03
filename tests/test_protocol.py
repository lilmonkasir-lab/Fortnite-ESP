import io
import unittest

from lua_injection.protocol import ProtocolError, decode_message, encode_message, read_message


class ProtocolTests(unittest.TestCase):
    def test_round_trip(self):
        message = {"type": "ping", "id": "one", "values": [1, True, None]}
        self.assertEqual(read_message(io.BytesIO(encode_message(message))), message)

    def test_rejects_non_object(self):
        with self.assertRaises(ProtocolError):
            decode_message(b"[1, 2]\n")

    def test_rejects_invalid_json(self):
        with self.assertRaises(ProtocolError):
            decode_message(b"not-json\n")


if __name__ == "__main__":
    unittest.main()
