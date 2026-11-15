"""Headless C/Python checks for the native LVUI user-question model."""
import importlib.util
import os
from pathlib import Path
import sys
import threading
import unittest

SOURCE = Path(__file__).resolve().parents[1]
MODULE = os.environ.get("IIQUESTION_TEST_PYTHON_MODULE", SOURCE / "src/python/iiquestion_presentation.py")
spec = importlib.util.spec_from_file_location("iiquestion_presentation", MODULE)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
LIBRARY = sys.argv.pop(1) if len(sys.argv) > 1 else None
if LIBRARY == "-":
    LIBRARY = None
CORE = sys.argv.pop(1) if len(sys.argv) > 1 else None


def request(identity, multiple=False):
    return {"id": identity, "questions": [{"text": "__proto__", "header": "Renderer", "multiple": multiple,
        "options": [{"label": "Native", "description": "Direct", "preview": "native preview"},
                    {"label": "Web", "description": "Browser", "preview": "web preview"}]}]}


class QuestionPresentationTest(unittest.TestCase):
    def model(self):
        model = module.QuestionModel(LIBRARY, core_library_dir=CORE)
        self.addCleanup(model.close)
        return model

    def test_drafts_queue_and_submission(self):
        model = self.model()
        events, actions = [], []
        model.observe(lambda name, row, option, generation: events.append((name, row, option, generation)))
        model.set_actions(lambda kind, identity, answers, annotations:
                          actions.append((kind, identity, answers, annotations)) or True)
        self.assertTrue(model.update([request("first", True)]))
        initial = model.snapshot()
        self.assertTrue(initial["open"])
        self.assertEqual(initial["rows"][0]["text"], "__proto__")
        generation = initial["generation"]
        model.select(generation, 0, 1, True)
        model.select(generation, 0, 0, True)
        model.preview(generation, 0, 1)
        self.assertEqual(model.snapshot()["rows"][0]["preview"], "web preview")
        model.edit_other(generation, 0, "Custom")
        model.edit_notes(generation, 0, "  Note  ")
        self.assertTrue(model.change("second", request("second"), index=1))
        current = model.snapshot()
        self.assertEqual(current["generation"], generation)
        self.assertTrue(current["has_waiting"])
        self.assertEqual(current["rows"][0]["other"], "Custom")
        self.assertTrue(any(event[0] == "option.checked" for event in events))
        self.assertEqual([item["id"] for item in model.read_requests()["requests"]], ["first", "second"])
        self.assertTrue(model.command("submit"))
        self.assertEqual(actions[-1], ("submit", "first", {"__proto__": "Native, Web, Custom"},
                                       {"__proto__": {"notes": "Note"}}))
        self.assertTrue(model.change("first", None))
        replacement = model.snapshot()
        self.assertNotEqual(replacement["generation"], generation)
        with self.assertRaisesRegex(RuntimeError, "stale"):
            model.edit_other(generation, 0, "must not affect second")
        generation = replacement["generation"]
        model.select(generation, 0, 0, True)
        self.assertTrue(model.command("submit"))
        self.assertEqual(actions[-1][3], {"__proto__": {"preview": "native preview"}})
        self.assertTrue(model.command("skip"))
        self.assertEqual(actions[-1], ("submit", "second", {}, {}))
        self.assertTrue(model.command("decline"))
        self.assertEqual(actions[-1], ("decline", "second", {}, {}))

    def test_validation_lifetime_and_threads(self):
        model = self.model()
        with self.assertRaises(ValueError):
            model.update([request("bad\0id")])
        with self.assertRaises(ValueError):
            model.update(request(str(index)) for index in range(65))
        with self.assertRaises(ValueError):
            model.update([{"id": "bad", "questions": [request("x")["questions"][0]] * 5}])
        model.update([request("first")])
        lease = model.lease_model()
        self.assertTrue(lease.pointer)
        errors = []
        def foreign():
            try:
                model.snapshot()
            except RuntimeError as error:
                errors.append(str(error))
        thread = threading.Thread(target=foreign)
        thread.start();thread.join()
        self.assertEqual(len(errors), 1)
        self.assertIn("owner thread", errors[0])
        failures = []
        def observe(name, _row, _option, _generation):
            if name == "error":
                try:
                    lease.close()
                except RuntimeError as error:
                    failures.append(str(error))
                model.close()
        model.observe(observe)
        model.set_error("temporary")
        self.assertIn("after callbacks return", failures[0])
        with self.assertRaises(RuntimeError):
            model.snapshot()
        self.assertTrue(lease.pointer)
        lease.close()

    def test_failed_callbacks_and_invalid_requests(self):
        model = self.model()
        with self.assertRaises(RuntimeError):
            model.update([{"id": "bad", "questions": []}])
        model.update([request("first")])
        def fail(*_args):
            raise ValueError("host failed")
        model.set_actions(fail)
        with self.assertRaisesRegex(ValueError, "host failed"):
            model.command("submit")
        self.assertTrue(model.snapshot()["open"])

    def test_localization_and_motion_are_change_driven(self):
        model = self.model()
        events = []
        model.observe(lambda name, row, option, generation: events.append(name))
        model.set_strings({"title": "질문", "waiting": "%1개 대기"})
        model.set_motion(False)
        model.update([request("first"), request("second")])
        state = model.snapshot()
        self.assertEqual(state["title"], "질문")
        self.assertEqual(state["waiting"], "2개 대기")
        self.assertFalse(state["motion_enabled"])
        self.assertIn("title", events)
        self.assertIn("motion_enabled", events)

    def test_replacement_during_a_row_notification_retires_the_old_row(self):
        model = self.model()
        model.update([request("first")])
        generation = model.snapshot()["generation"]
        def observer(name, _row, _option, _generation):
            if name == "option.checked":
                model.update([request("next")])
        model.observe(observer)
        model.select(generation, 0, 0, True)
        state = model.snapshot()
        self.assertNotEqual(state["generation"], generation)
        self.assertFalse(state["rows"][0]["options"][0]["checked"])
        self.assertEqual(model.read_requests()["requests"][0]["id"], "next")


if __name__ == "__main__":
    unittest.main()
