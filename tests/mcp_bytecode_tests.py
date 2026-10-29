from pathlib import Path
import importlib.util,importlib.machinery,tempfile,unittest,sys
spec=importlib.util.spec_from_file_location('prepare_mcp_bytecode',Path(__file__).resolve().parents[1]/'tools/prepare_mcp_bytecode.py')
helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
class CacheContracts(unittest.TestCase):
 def test_compiled_dependencies_remain_executable_and_update(self):
  with tempfile.TemporaryDirectory(dir=Path.cwd()) as folder:
   prefix=Path(folder);library=prefix/'site-packages';library.mkdir();source=library/'module.py';source.write_text('value=7\n')
   self.assertEqual(helper.prepare(prefix,[library])['sources'],1)
   cache=helper.cache_file(source);self.assertTrue(cache.is_file())
   def load():
    loader=importlib.machinery.SourcelessFileLoader('compiled_test',str(cache));module=importlib.util.module_from_spec(importlib.util.spec_from_loader(loader.name,loader));loader.exec_module(module);return module.value
   self.assertEqual(load(),7);source.write_text('value=8000\n');helper.prepare(prefix,[library]);self.assertEqual(load(),8000)
 def test_compilation_never_executes_dependency_code(self):
  with tempfile.TemporaryDirectory(dir=Path.cwd()) as folder:
   prefix=Path(folder);library=prefix/'packages';library.mkdir();(library/'bad.py').write_text('raise RuntimeError("must not run during preparation")\n')
   self.assertEqual(helper.prepare(prefix,[library])['sources'],1)
 def test_outside_roots_and_symlinks_do_not_receive_artifacts(self):
  with tempfile.TemporaryDirectory(dir=Path.cwd()) as folder:
   root=Path(folder);prefix=root/'managed';library=prefix/'packages';library.mkdir(parents=True);outside=root/'outside.py';outside.write_text('value=1\n')
   (library/'link.py').symlink_to(outside)
   self.assertEqual(helper.prepare(prefix,[library])['skipped_outside'],1);self.assertFalse(helper.cache_file(outside).exists())
   with self.assertRaises(ValueError):helper.prepare(prefix,[root])
 def test_interpreter_cache_prefix_cannot_redirect_managed_outputs(self):
  with tempfile.TemporaryDirectory(dir=Path.cwd()) as folder:
   root=Path(folder);prefix=root/'managed';library=prefix/'packages';library.mkdir(parents=True);source=library/'module.py';source.write_text('value=7\n');outside=root/'outside'
   previous=sys.pycache_prefix
   try:
    sys.pycache_prefix=str(outside);helper.prepare(prefix,[library])
   finally:sys.pycache_prefix=previous
   self.assertTrue(helper.cache_file(source).is_file());self.assertFalse(outside.exists())
if __name__=='__main__':unittest.main()
