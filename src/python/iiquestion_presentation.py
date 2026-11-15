"""Owner-thread Python access to the native LVUI user-question model."""
from __future__ import annotations

import ctypes
import itertools
import os
from pathlib import Path
import sys
import threading
import weakref

_NAMES = ("rows", "open", "has_waiting", "has_error", "motion_enabled", "title", "description", "waiting", "error",
          "other_placeholder", "notes_placeholder", "decline_text", "skip_text", "send_text",
          "row.id", "row.text", "row.header", "row.multiple", "row.other", "row.notes", "row.preview",
          "row.other_accessibility", "row.notes_accessibility", "option.id", "option.label", "option.description",
          "option.preview", "option.checked")
_IDS = {name: index for index, name in enumerate(_NAMES)}
_COMMANDS = {"submit": 0, "skip": 1, "decline": 2}
_GLOBAL_TEXT = _NAMES[5:14]
_ROW_TEXT = ("id", "text", "header", "other", "notes", "preview", "other_accessibility", "notes_accessibility")
_OPTION_TEXT = ("id", "label", "description", "preview")
_NO_INDEX = ctypes.c_size_t(-1).value
_LOAD_LOCK = threading.Lock()
_LIBRARIES = {}
_DLL_DIRECTORIES = {}


class _Option(ctypes.Structure):
    _fields_ = [(name, ctypes.c_char_p) for name in ("label", "description", "preview")]


class _Question(ctypes.Structure):
    _fields_ = [("text", ctypes.c_char_p), ("header", ctypes.c_char_p), ("multiple", ctypes.c_int),
                ("options", ctypes.POINTER(_Option)), ("option_count", ctypes.c_size_t)]


class _Request(ctypes.Structure):
    _fields_ = [("id", ctypes.c_char_p), ("questions", ctypes.POINTER(_Question)), ("question_count", ctypes.c_size_t)]


class _Strings(ctypes.Structure):
    _fields_ = [(name, ctypes.c_char_p) for name in ("title", "description", "waiting", "other", "notes",
               "other_accessibility", "notes_accessibility", "decline", "skip", "send")]


class _ResponseItem(ctypes.Structure):
    _fields_ = [(name, ctypes.c_char_p) for name in ("question", "answer", "preview", "notes")]


_OBSERVER = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_int, ctypes.c_size_t, ctypes.c_size_t, ctypes.c_uint64)
_ACTION = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p,
                           ctypes.POINTER(_ResponseItem), ctypes.c_size_t)
_READ = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.POINTER(_Request), ctypes.c_size_t, ctypes.c_char_p)


def _utf8(value):
    if not isinstance(value, str):
        raise TypeError("question text must be a string")
    if "\0" in value:
        raise ValueError("question text cannot contain NUL")
    encoded = value.encode("utf-8")
    if len(encoded) > 131072:
        raise ValueError("question text is too long")
    return encoded


def _boolean(value):
    if not isinstance(value, bool):
        raise TypeError("question flag must be Boolean")
    return int(value)


def _index(value):
    if not isinstance(value, int) or isinstance(value, bool) or not 0 <= value < _NO_INDEX:
        raise ValueError("question index must be a non-negative native size")
    return value


def _generation(value):
    if not isinstance(value, int) or isinstance(value, bool) or not 0 <= value <= (1 << 64) - 1:
        raise ValueError("invalid question generation")
    return value


def _bounded(values, limit, name):
    if isinstance(values, (str, bytes, dict)):
        raise TypeError(name + " must be an iterable of objects")
    result = list(itertools.islice(iter(values), limit + 1))
    if len(result) > limit:
        raise ValueError("too many " + name)
    return result


def _encode_request(value, keep):
    if not isinstance(value, dict) or set(value) - {"id", "questions"}:
        raise ValueError("invalid question request fields")
    questions = []
    for question in _bounded(value.get("questions", []), 4, "questions"):
        if not isinstance(question, dict) or set(question) - {"text", "header", "multiple", "options"}:
            raise ValueError("invalid question fields")
        options = []
        for option in _bounded(question.get("options", []), 4, "options"):
            if not isinstance(option, dict) or set(option) - {"label", "description", "preview"}:
                raise ValueError("invalid question option fields")
            options.append(_Option(*(_utf8(option.get(name, "")) for name in ("label", "description", "preview"))))
        encoded_options = (_Option * len(options))(*options)
        keep.append(encoded_options)
        questions.append(_Question(_utf8(question.get("text", "")), _utf8(question.get("header", "")),
                                   _boolean(question.get("multiple", False)), encoded_options, len(encoded_options)))
    encoded_questions = (_Question * len(questions))(*questions)
    keep.append(encoded_questions)
    result = _Request(_utf8(value.get("id", "")), encoded_questions, len(encoded_questions))
    keep.append(result)
    return result


def _decode_request(value):
    questions = []
    for index in range(value.question_count):
        question = value.questions[index]
        questions.append({"text": question.text.decode("utf-8"), "header": question.header.decode("utf-8"),
                          "multiple": bool(question.multiple), "options": [
                              {name: getattr(question.options[j], name).decode("utf-8")
                               for name in ("label", "description", "preview")}
                              for j in range(question.option_count)]})
    return {"id": value.id.decode("utf-8"), "questions": questions}


def _library_path():
    configured = os.environ.get("IIQUESTION_PRESENTATION_LIBRARY")
    if configured:
        return configured
    prefix = Path(__file__).resolve().parents[3]
    names = (("iiMultimodalAgent.dll", "libiiMultimodalAgent.dll") if sys.platform == "win32" else
             ("libiiMultimodalAgent.dylib",) if sys.platform == "darwin" else ("libiiMultimodalAgent.so",))
    for directory in ("lib", "lib64", "bin"):
        for name in names:
            candidate = prefix / directory / name
            if candidate.is_file():
                return str(candidate)
    raise OSError("Cannot find the iiMultimodalAgent shared library; provide library_path")


def _load_library(path, core_directory):
    def load(value, globally=False):
        mode = getattr(ctypes, "RTLD_GLOBAL", 0) if globally else getattr(ctypes, "RTLD_LOCAL", 0)
        key = (str(value), mode)
        if key not in _LIBRARIES:
            _LIBRARIES[key] = ctypes.CDLL(str(value), mode=mode)
        return _LIBRARIES[key]
    with _LOAD_LOCK:
        if core_directory:
            directory = Path(core_directory).expanduser().resolve(strict=True)
            if not directory.is_dir():
                raise NotADirectoryError(str(directory))
            if sys.platform == "win32" and str(directory) not in _DLL_DIRECTORIES:
                _DLL_DIRECTORIES[str(directory)] = os.add_dll_directory(str(directory))
            for name in ("iiSDKCoreObservation", "iiSDKCore"):
                names = ((name + ".dll", "lib" + name + ".dll") if sys.platform == "win32" else
                         ("lib" + name + ".dylib",) if sys.platform == "darwin" else ("lib" + name + ".so",))
                candidate = next((directory / filename for filename in names if (directory / filename).is_file()), None)
                if candidate:
                    load(candidate.resolve(), globally=True)
                elif name == "iiSDKCore":
                    raise OSError("No shared iiSDKCore library in " + str(directory))
        candidate = Path(path).expanduser()
        if candidate.is_file():
            candidate = candidate.resolve()
            if sys.platform == "win32" and str(candidate.parent) not in _DLL_DIRECTORIES:
                _DLL_DIRECTORIES[str(candidate.parent)] = os.add_dll_directory(str(candidate.parent))
            return load(candidate)
        return load(path)


def _configure(library):
    v, i, s, z, u = ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_uint64
    signatures = {
        "last_error": ([], s), "create": ([], v), "destroy": ([v], i), "observe": ([v, v, v], i),
        "set_actions": ([v, v, v], i), "set_strings": ([v, ctypes.POINTER(_Strings)], i),
        "update": ([v, ctypes.POINTER(_Request), z, s, ctypes.POINTER(i)], i),
        "change": ([v, s, ctypes.POINTER(_Request), z, ctypes.POINTER(i)], i),
        "set_error": ([v, s, ctypes.POINTER(i)], i), "read": ([v, v, v], i),
        "rows": ([v, ctypes.POINTER(u), ctypes.POINTER(z)], i), "boolean": ([v, i, ctypes.POINTER(i)], i),
        "set_motion": ([v, i], i), "text": ([v, i, v, z, ctypes.POINTER(z)], i),
        "row_boolean": ([v, u, z, i, ctypes.POINTER(i)], i), "row_text": ([v, u, z, i, v, z, ctypes.POINTER(z)], i),
        "options": ([v, u, z, ctypes.POINTER(z)], i),
        "option_boolean": ([v, u, z, z, i, ctypes.POINTER(i)], i),
        "option_text": ([v, u, z, z, i, v, z, ctypes.POINTER(z)], i),
        "select": ([v, u, z, z, i], i), "preview": ([v, u, z, z], i), "edit": ([v, u, z, i, s], i),
        "command": ([v, i, ctypes.POINTER(i)], i), "acquire_model": ([v, ctypes.POINTER(v)], i),
        "lease_model": ([v, ctypes.POINTER(v)], i), "release_model": ([v], i),
    }
    for name, (arguments, result) in signatures.items():
        function = getattr(library, "iiquestion_" + name)
        function.argtypes, function.restype = arguments, result


class QuestionModel:
    """Owner-thread question UI state, queries and explicit host callbacks."""
    def __init__(self, library_path=None, *, core_library_dir=None):
        core_library_dir = core_library_dir or os.environ.get("IIQUESTION_CORE_LIBRARY_DIR")
        self._library = _load_library(str(library_path or _library_path()), core_library_dir)
        _configure(self._library)
        self._owner = threading.get_ident()
        self._depth = self._callback_depth = 0
        self._closed = self._pending_close = False
        self._callback_errors = []
        self._observer = self._action = None
        self._handle = self._library.iiquestion_create()
        if not self._handle:
            raise RuntimeError(self._error())
        weak = weakref.ref(self)

        def on_observe(_context, property_id, row, option, generation):
            model = weak()
            if model is None or model._closed or model._observer is None:
                return
            model._callback_depth += 1
            try:
                model._observer(_NAMES[property_id], None if row == _NO_INDEX else row,
                                None if option == _NO_INDEX else option, generation)
            except BaseException as exc:
                model._callback_errors.append(exc)
            finally:
                model._callback_depth -= 1

        def on_action(_context, kind, identity, values, count):
            model = weak()
            if model is None or model._closed or model._action is None:
                return 0
            model._callback_depth += 1
            try:
                answers, annotations = {}, {}
                for index in range(count):
                    item = values[index]
                    question = item.question.decode("utf-8")
                    if item.answer is not None:
                        answers[question] = item.answer.decode("utf-8")
                    annotation = {}
                    if item.preview is not None:
                        annotation["preview"] = item.preview.decode("utf-8")
                    if item.notes is not None:
                        annotation["notes"] = item.notes.decode("utf-8")
                    if annotation:
                        annotations[question] = annotation
                accepted = model._action(("submit", "decline")[kind], identity.decode("utf-8"), answers, annotations)
                return 1 if accepted is True else 0
            except BaseException as exc:
                model._callback_errors.append(exc)
                return 0
            finally:
                model._callback_depth -= 1
        self._observer_callback = _OBSERVER(on_observe)
        self._action_callback = _ACTION(on_action)

    def _error(self):
        return (self._library.iiquestion_last_error() or b"native question model error").decode("utf-8", "replace")

    def _check_owner(self):
        if threading.get_ident() != self._owner:
            raise RuntimeError("question model requires its owner thread")

    def _check_open(self):
        self._check_owner()
        if self._closed:
            raise RuntimeError("question model is closed")

    def _destroy(self):
        if self._handle:
            if self._library.iiquestion_destroy(self._handle):
                raise RuntimeError(self._error())
            self._handle = None
        self._pending_close = False
        self._observer = self._action = None

    def _call(self, function, *args):
        self._check_open()
        self._depth += 1
        try:
            if function(self._handle, *args):
                raise RuntimeError(self._error())
        finally:
            self._depth -= 1
            if not self._depth and self._pending_close:
                self._destroy()
        if not self._depth and not self._callback_depth:
            self.raise_callback_errors()

    def raise_callback_errors(self):
        self._check_owner()
        if self._callback_errors:
            raise self._callback_errors.pop(0)

    def close(self):
        self._check_owner()
        if self._closed:
            return
        self._closed = True
        if self._depth:
            self._pending_close = True
        else:
            self._destroy()

    def __enter__(self):
        self._check_open()
        return self

    def __exit__(self, *_):
        self.close()

    def __del__(self):
        if (getattr(self, "_handle", None) and not getattr(self, "_closed", True)
                and threading.get_ident() == getattr(self, "_owner", None)):
            try:
                self.close()
            except Exception:
                pass

    def observe(self, callback):
        if callback is not None and not callable(callback):
            raise TypeError("observer must be callable or None")
        self._check_open()
        self._observer = callback
        self._call(self._library.iiquestion_observe,
                   ctypes.cast(self._observer_callback, ctypes.c_void_p) if callback else None, None)

    def set_actions(self, callback):
        if callback is not None and not callable(callback):
            raise TypeError("action handler must be callable or None")
        self._check_open()
        self._action = callback
        self._call(self._library.iiquestion_set_actions,
                   ctypes.cast(self._action_callback, ctypes.c_void_p) if callback else None, None)

    def update(self, requests, error=""):
        keep = []
        values = [_encode_request(value, keep) for value in _bounded(requests, 64, "requests")]
        encoded = (_Request * len(values))(*values)
        result = ctypes.c_int()
        self._call(self._library.iiquestion_update, encoded, len(encoded), _utf8(error), ctypes.byref(result))
        return bool(result.value)

    def change(self, identity, value, *, index=0):
        keep, result = [], ctypes.c_int()
        encoded = _encode_request(value, keep) if value is not None else None
        self._call(self._library.iiquestion_change, _utf8(identity), ctypes.byref(encoded) if encoded else None,
                   _index(index), ctypes.byref(result))
        return bool(result.value)

    def set_error(self, error):
        result = ctypes.c_int()
        self._call(self._library.iiquestion_set_error, _utf8(error), ctypes.byref(result))
        return bool(result.value)

    def set_motion(self, enabled):
        self._call(self._library.iiquestion_set_motion, _boolean(enabled))

    def set_strings(self, values):
        defaults = {"title": "Agent questions", "description": "Choose an option or write your own answer. You can leave questions unanswered.",
                    "waiting": "%1 requests waiting", "other": "Your own answer", "notes": "Additional notes (optional)",
                    "other_accessibility": "Your answer to %1", "notes_accessibility": "Notes for %1",
                    "decline": "Decline", "skip": "Skip", "send": "Send answers"}
        if not isinstance(values, dict) or set(values) - set(defaults):
            raise ValueError("invalid question string fields")
        defaults.update(values)
        strings = _Strings(*(_utf8(defaults[name]) for name, _ in _Strings._fields_))
        self._call(self._library.iiquestion_set_strings, ctypes.byref(strings))

    def read_requests(self):
        result = {}
        def read(_context, values, count, message):
            try:
                result.update(requests=[_decode_request(values[index]) for index in range(count)], error=message.decode("utf-8"))
            except BaseException as exc:
                self._callback_errors.append(exc)
        callback = _READ(read)
        self._call(self._library.iiquestion_read, ctypes.cast(callback, ctypes.c_void_p), None)
        return result

    def _text(self, function, *prefix):
        length = ctypes.c_size_t()
        self._call(function, *prefix, None, 0, ctypes.byref(length))
        buffer = ctypes.create_string_buffer(length.value + 1)
        self._call(function, *prefix, buffer, len(buffer), ctypes.byref(length))
        return buffer.value.decode("utf-8")

    def _scalar(self, function, kind, *prefix):
        result = kind()
        self._call(function, *prefix, ctypes.byref(result))
        return result.value

    def snapshot(self):
        lib = self._library
        generation, count = ctypes.c_uint64(), ctypes.c_size_t()
        self._call(lib.iiquestion_rows, ctypes.byref(generation), ctypes.byref(count))
        gen = generation.value
        result = {"generation": gen}
        result.update((name, bool(self._scalar(lib.iiquestion_boolean, ctypes.c_int, _IDS[name])))
                      for name in ("open", "has_waiting", "has_error", "motion_enabled"))
        result.update((name, self._text(lib.iiquestion_text, _IDS[name])) for name in _GLOBAL_TEXT)
        rows = []
        for index in range(count.value):
            row = {name: self._text(lib.iiquestion_row_text, gen, index, _IDS["row." + name]) for name in _ROW_TEXT}
            row["multiple"] = bool(self._scalar(lib.iiquestion_row_boolean, ctypes.c_int, gen, index, _IDS["row.multiple"]))
            option_count = self._scalar(lib.iiquestion_options, ctypes.c_size_t, gen, index)
            row["options"] = []
            for option in range(option_count):
                item = {name: self._text(lib.iiquestion_option_text, gen, index, option, _IDS["option." + name])
                        for name in _OPTION_TEXT}
                item["checked"] = bool(self._scalar(lib.iiquestion_option_boolean, ctypes.c_int,
                                                     gen, index, option, _IDS["option.checked"]))
                row["options"].append(item)
            rows.append(row)
        result["rows"] = rows
        return result

    def select(self, generation, row, option, selected):
        self._call(self._library.iiquestion_select, _generation(generation), _index(row), _index(option), _boolean(selected))

    def preview(self, generation, row, option):
        self._call(self._library.iiquestion_preview, _generation(generation), _index(row), _index(option))

    def edit_other(self, generation, row, value):
        self._call(self._library.iiquestion_edit, _generation(generation), _index(row), _IDS["row.other"], _utf8(value))

    def edit_notes(self, generation, row, value):
        self._call(self._library.iiquestion_edit, _generation(generation), _index(row), _IDS["row.notes"], _utf8(value))

    def command(self, name):
        if name not in _COMMANDS:
            raise ValueError("unknown question command: " + str(name))
        result = ctypes.c_int()
        self._call(self._library.iiquestion_command, _COMMANDS[name], ctypes.byref(result))
        return bool(result.value)

    def lease_model(self):
        lease = ctypes.c_void_p()
        try:
            self._call(self._library.iiquestion_acquire_model, ctypes.byref(lease))
            return ModelLease(self, lease.value)
        except BaseException:
            if lease.value:
                self._library.iiquestion_release_model(lease.value)
            raise


class ModelLease:
    """Retain the native model until its view and active queries have retired."""
    def __init__(self, model, handle):
        self._model, self._handle = model, handle

    def _call(self, function, *args):
        self._model._check_owner()
        if not self._handle:
            raise RuntimeError("question model lease is closed")
        if function(self._handle, *args):
            raise RuntimeError(self._model._error())

    @property
    def pointer(self):
        result = ctypes.c_void_p()
        self._call(self._model._library.iiquestion_lease_model, ctypes.byref(result))
        return result.value

    def close(self):
        self._model._check_owner()
        if self._handle:
            self._call(self._model._library.iiquestion_release_model)
            self._handle = None

    def __enter__(self):
        self._model._check_owner()
        if not self._handle:
            raise RuntimeError("question model lease is closed")
        return self

    def __exit__(self, *_):
        self.close()

    def __del__(self):
        model = getattr(self, "_model", None)
        if (getattr(self, "_handle", None) and model is not None
                and threading.get_ident() == getattr(model, "_owner", None)):
            try:
                self.close()
            except Exception:
                pass
