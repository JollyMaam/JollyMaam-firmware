#!/usr/bin/env python3
"""Tests of stress.py's parts that need no device: plans, summaries, outages."""
import itertools
import os
import sys
import threading
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stress  # noqa: E402


class FakeHost:
    """stress.Host's state, filled by hand."""

    def __init__(self, times, lost=(), attached=()):
        self.times, self.lost, self.lock = list(times), list(lost), threading.Lock()
        self.attached = [(x, "hidraw0") for x in attached]

    def since(self, t):
        return [x for x in self.times if x >= t]

    def attached_after(self, t):
        return [x for x, _ in self.attached if x >= t]


class FakeTarget:
    def __init__(self, host, link="dongle"):
        self.host, self.link = host, link


class Plan(unittest.TestCase):
    def test_parse_kv(self):
        self.assertEqual(stress.parse_kv("pad=5-2.2,link=usb, slot = east"),
                         {"pad": "5-2.2", "link": "usb", "slot": "east"})

    def test_typed(self):
        p = stress.typed({"test": "connect", "count": "5", "hold": "2"}, stress.DEFAULTS["connect"])
        self.assertEqual(p["count"], 5)
        self.assertIsInstance(p["count"], int)
        self.assertEqual(p["hold"], 2.0)
        self.assertEqual(p["gap"], 2)
        with self.assertRaises(SystemExit):
            stress.typed({"cuont": 5}, stress.DEFAULTS["connect"])

    def test_example_tests_known(self):
        for x in stress.EXAMPLE["tests"]:
            self.assertIn(x["test"], stress.TESTS)
            stress.typed(x, stress.DEFAULTS[x["test"]])


class Summaries(unittest.TestCase):
    def test_quantiles(self):
        q = stress.quantiles(list(range(1, 101)))
        self.assertEqual((q["p50"], q["p99"], q["max"], q["n"]), (51, 100, 100, 100))
        self.assertIsNone(stress.quantiles([]))

    def test_connect_fail_counts(self):
        rows = [{"cycle": 0, "ok": True, "connect_s": 1.0, "disconnect_s": 0.5, "rate": 100,
                 "gap_ms": {"p50": 8, "p90": 9, "p99": 10, "max": 12, "n": 9}},
                {"cycle": 1, "ok": False, "why": "no report from the host"}]
        verdict, text = stress.summarize("t", "connect", rows)
        self.assertEqual(verdict, "FAIL")
        self.assertIn("1/2 ok", text)

    def test_skip_and_error(self):
        self.assertEqual(stress.summarize("t", "latency", [{"skipped": "no probe"}]), ("skip", "no probe"))
        self.assertEqual(stress.summarize("t", "latency", [{"error": "boom"}])[0], "FAIL")

    def test_soak_windows_not_cycles(self):
        rows = [{"event": "window", "rate": 100}, {"ok": True, "seconds": 10, "drive": "idle", "drops": 0,
                                                    "stalls": 2, "worst_stalls_ms": [150, 300], "rate": 99,
                                                    "gap_ms": {"p50": 8, "p90": 9, "p99": 10, "max": 300}}]
        verdict, text = stress.summarize("t", "soak", rows)
        self.assertEqual(verdict, "ok")
        self.assertIn("links lost 0", text)


class Outage(unittest.TestCase):
    def setUp(self):
        self.real_now = stress.now

    def tearDown(self):
        stress.now = self.real_now

    def run_outage(self, times, lost, at, attached=(), link="dongle"):
        stress.now = lambda: at
        return stress.outage(FakeTarget(FakeHost(times, lost, attached), link), 10.0, 1)

    def test_gap(self):
        times = [9.9 + i * 0.01 for i in range(20)] + [11.6 + i * 0.01 for i in range(20)]
        o, dropped = self.run_outage(times, [], 12.0)
        self.assertTrue(dropped)
        self.assertAlmostEqual(o, 11.6 - 10.09, places=3)

    def test_node_lost(self):
        times = [9.9 + i * 0.01 for i in range(40)] + [11.0]
        o, dropped = self.run_outage(times, [10.4], 12.0)
        self.assertTrue(dropped)
        self.assertAlmostEqual(o, 11.0 - 10.29, places=3)

    def test_bluetooth_node_back(self):
        # An idle pad over Bluetooth: no report after the drop, only the node back.
        times = [9.9 + i * 0.01 for i in range(40)]
        o, dropped = self.run_outage(times, [10.4], 12.0, attached=[9.0, 11.5], link="bluetooth")
        self.assertTrue(dropped)
        self.assertAlmostEqual(o, 11.5 - 10.29, places=3)

    def test_no_drop(self):
        clock = itertools.chain([10.0], itertools.repeat(16.0))     # past the 5 s wait for a drop
        stress.now = lambda: next(clock)
        times = [9.9 + i * 0.01 for i in range(600)]
        self.assertEqual(stress.outage(FakeTarget(FakeHost(times)), 10.0, 20), (None, False))


if __name__ == "__main__":
    unittest.main()
