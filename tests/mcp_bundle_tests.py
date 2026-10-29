from pathlib import Path
import importlib.util,json,os,subprocess,sys,tempfile,unittest,zipfile
ROOT=Path(__file__).resolve().parents[1]
def load(name,path):
 spec=importlib.util.spec_from_file_location(name,path);module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
bundle=load('bundle_source',ROOT/'tools/mcp_dependency_bundle.py')
bytecode=load('bytecode_source',ROOT/'tools/prepare_mcp_bytecode.py')
class BundleContracts(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory(dir=Path.cwd());self.root=Path(self.temp.name);self.lib=self.root/'managed/packages';self.lib.mkdir(parents=True)
  self.package=self.lib/'fixture_dependency';self.package.mkdir();(self.package/'__init__.py').write_text('from importlib.resources import files\nVALUE=7\ndef data(): return (files(__package__)/"data.txt").read_text()\n');(self.package/'data.txt').write_text('resource bytes')
 def tearDown(self):self.temp.cleanup()
 def prepare(self):
  bytecode.prepare(self.root/'managed',[self.lib]);return bundle.build(self.root/'managed',self.lib)
 def child(self):
  code='import _ii_sdk_mcp_bundle as cache; active=cache.activate('+repr(str(self.lib))+'); import fixture_dependency as dep,json;print(json.dumps([active,dep.VALUE,dep.data(),dep.__file__]))'
  env=os.environ.copy();env['PYTHONPATH']=str(self.lib);env.pop('PYTHONPYCACHEPREFIX',None)
  return json.loads(subprocess.check_output([sys.executable,'-B','-c',code],env=env,text=True))
 def test_zip_import_preserves_code_resources_and_native_sources(self):
  self.prepare();active,value,data,origin=self.child();self.assertTrue(active);self.assertEqual(value,7);self.assertEqual(data,'resource bytes');self.assertIn('dependencies.zip',origin)
  self.assertEqual((self.package/'data.txt').read_text(),'resource bytes')
 def test_source_change_falls_back_to_current_files(self):
  self.prepare();source=self.package/'__init__.py';source.write_text(source.read_text().replace('VALUE=7','VALUE=90000'))
  active,value,data,origin=self.child();self.assertFalse(active);self.assertEqual(value,90000);self.assertNotIn('dependencies.zip',origin)
 def test_new_source_and_data_directory_entries_invalidate_the_snapshot(self):
  self.prepare();(self.package/'new_resource.txt').write_text('new');self.assertFalse(self.child()[0])
 def test_native_extension_packages_stay_on_the_filesystem(self):
  native=self.lib/'native_fixture';native.mkdir();(native/'__init__.py').write_text('VALUE=1\n');(native/'extension.so').write_bytes(b'native placeholder')
  result=self.prepare();self.assertIn('native_fixture',result['excluded_native_packages'])
  with zipfile.ZipFile(self.lib/bundle.CACHE/'dependencies.zip') as archive:self.assertFalse(any(name.startswith('native_fixture/') for name in archive.namelist()))
 def test_corrupted_archive_never_becomes_an_import_source(self):
  self.prepare();(self.lib/bundle.CACHE/'dependencies.zip').write_bytes(b'broken');self.assertFalse(self.child()[0])
 def test_external_symlinks_are_not_bundled(self):
  outside=self.root/'outside.py';outside.write_text('PRIVATE=1\n');(self.package/'outside.py').symlink_to(outside);self.prepare()
  with zipfile.ZipFile(self.lib/bundle.CACHE/'dependencies.zip') as archive:self.assertNotIn('fixture_dependency/outside.py',archive.namelist())
  self.assertEqual(outside.read_text(),'PRIVATE=1\n')
 def test_unicode_library_paths_use_locale_independent_startup_configuration(self):
  target=self.root/'managed'/'한국어 경로'/'packages';target.parent.mkdir();self.lib.rename(target);self.lib=target;self.package=self.lib/'fixture_dependency'
  self.prepare();self.assertTrue((self.lib/bundle.PTH).read_bytes().isascii());self.assertTrue(self.child()[0])
 def test_unmanaged_library_and_oversized_artifact_are_rejected(self):
  with self.assertRaises(ValueError):bundle.build(self.root/'managed',self.root)
  self.prepare();self.assertEqual(bundle.build(self.root/'managed',self.lib,max_bytes=1)['status'],'too_large');self.assertFalse((self.lib/bundle.PTH).exists())
if __name__=='__main__':unittest.main()
