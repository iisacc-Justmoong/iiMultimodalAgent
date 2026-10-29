"""Prepare ordinary CPython dependency artifacts in a managed SDK environment."""
from pathlib import Path
import importlib.util,json,os,py_compile,struct,sys,sysconfig

def cache_file(source):
    source=Path(source)
    return source.parent/'__pycache__'/(source.stem+'.'+sys.implementation.cache_tag+'.pyc')

def current_timestamp_artifact(source):
    try:
        stat=source.stat()
        expected=struct.pack('<4sIII',importlib.util.MAGIC_NUMBER,0,int(stat.st_mtime)&0xffffffff,stat.st_size&0xffffffff)
        with cache_file(source).open('rb') as cache:
            return cache.read(16)==expected
    except OSError:return False

def prepare(prefix, libraries):
    prefix=Path(prefix).resolve();sources=0;outside=0
    for library in sorted({Path(p).resolve() for p in libraries}):
        if not library.is_relative_to(prefix):raise ValueError("Library is outside the managed prefix")
        for directory,children,files in os.walk(library,followlinks=False):
            children[:]=[x for x in children if x!='__pycache__' and not (Path(directory)/x).is_symlink()]
            for name in files:
                if not name.endswith('.py'):continue
                source=Path(directory)/name
                if not source.resolve().is_relative_to(prefix):outside+=1;continue
                if not cache_file(source).resolve().is_relative_to(prefix):outside+=1;continue
                if not current_timestamp_artifact(source):
                    py_compile.compile(str(source),cfile=str(cache_file(source)),doraise=True,optimize=0,
                        invalidation_mode=py_compile.PycInvalidationMode.TIMESTAMP)
                sources+=1
    return {'status':'prepared','sources':sources,'skipped_outside':outside}

def main():
    prefix=Path(sys.prefix).resolve();owner=Path(__file__).resolve().parents[2]
    if sys.prefix==sys.base_prefix or not prefix.is_relative_to(owner):
        print(json.dumps({'status':'unmanaged_prefix'}));return
    libraries=sorted({sysconfig.get_path('purelib'),sysconfig.get_path('platlib')})
    result=prepare(prefix,libraries)
    from mcp_dependency_bundle import build
    result['bundles']=[build(prefix,library) for library in libraries]
    print(json.dumps(result))
if __name__=='__main__':main()
