"""tests/mock_radio.py: the messages written as the standards (and "The 1090 Megahertz Riddle"'s examples) have them.
The screen's own decoder reads them back in the host tests (test_host.cpp test_mock_radio)."""

import unittest

import mock_radio as r


class Messages(unittest.TestCase):
    def test_the_riddles_examples(self):
        # Identification: KLM1023, no category given.
        self.assertEqual(r.identification(0x4840D6, "KLM1023", ""), "8D4840D6202CC371C32CE0576098")
        # An even airborne position at 38000 ft, its CPR fields as the example gives them.
        me = r.Bits().put(11, 5).put(0, 2).put(0, 1).put(r.altitude12(38000), 12).put(0, 1).put(0, 1)
        self.assertEqual(r.squitter(0x40621D, me.put(93000, 17).put(51372, 17)), "8D40621D58C382D690C8AC2863A7")

    def test_cpr_matches_the_example(self):
        # The example's place (52.2572, 3.9194) encodes to within a step of its fields.
        y, x = r.cpr(52.25720214843750, 3.91937255859375, odd=False)
        self.assertLessEqual(abs(y - 93000), 1)
        self.assertLessEqual(abs(x - 51372), 1)

    def test_checksums_hold(self):
        for p in r.SAMPLES:
            address = int(p["hex"], 16)
            for m in r.messages(p, False) + r.messages(p, True):
                data = bytes.fromhex(m)
                residue = r.crc(data[:-3]) ^ int.from_bytes(data[-3:], "big")
                self.assertEqual(residue, address if data[0] >> 3 == 5 else 0, m)

    def test_squawk_bits(self):
        # 7700: A=7, B=7, C=D=0: every A and B bit set, the C and D clear (and X).
        self.assertEqual(r.identity13("7700"), 0b0101010101010)
        self.assertEqual(r.identity13("0000"), 0)
        self.assertEqual(r.identity13("0007"), 0b0000000010101)

    def test_what_each_plane_sends(self):
        kinds = lambda p, odd: [(int(m[:2], 16) >> 3, int(m[8:10], 16) >> 3 if len(m) == 28 else None)
                                for m in r.messages(p, odd)]
        self.assertEqual(kinds(r.SAMPLES[0], False), [(17, 4), (17, 11), (17, 19), (5, None)])
        # An emergency besides, and no identification for a plane without a callsign.
        self.assertIn((17, 28), kinds(r.SAMPLES[1], True))
        self.assertNotIn((17, 4), kinds(r.SAMPLES[4], False))
        # Even then odd.
        even, odd = r.messages(r.SAMPLES[0], False)[1], r.messages(r.SAMPLES[0], True)[1]
        self.assertEqual((int(even[8:22], 16) >> 34) & 1, 0)
        self.assertEqual((int(odd[8:22], 16) >> 34) & 1, 1)


if __name__ == "__main__":
    unittest.main()
