"""Bundle integrity and native export tests using explicitly synthetic test ELF files."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile

source=Path(__file__).resolve().parents[2]
sys.path[:0]=[str(source/'tests/node'),str(source/'scripts/desktop')]
from bundle_fixture import make_bundle
spec=importlib.util.spec_from_file_location('resources',source/'scripts/desktop/remote_resources.py')
resources=importlib.util.module_from_spec(spec);spec.loader.exec_module(resources)
with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as folder:
 root=Path(folder); fixture=make_bundle(root/'fixture'); archives=root/'archives'; archives.mkdir()
 for arch in resources.ARCHES:
  with zipfile.ZipFile(archives/f'asterion-services-linux-{arch}.zip','w') as archive:
   for file in (fixture/arch).rglob('*'):
    if file.is_file(): archive.write(file,file.relative_to(fixture/arch).as_posix())
 try: resources.files_for('arm64')
 except ValueError: pass
 else: raise AssertionError('unsupported Linux ARM64 accepted')
 target=root/'staged'; resources.stage(archives,target); resources.verify(target)
 # Reused staging must reject and then remove obsolete resource trees.
 (target/'x86_64/plugins/obsolete.so').write_bytes(b'obsolete')
 (target/'retired-arch').mkdir()
 try: resources.verify(target)
 except ValueError: pass
 else: raise AssertionError('obsolete bundled resource accepted')
 resources.stage(archives,target); resources.verify(target)
 assert not (target/'x86_64/plugins/obsolete.so').exists()
 assert not (target/'retired-arch').exists()
 # A bundle of another product version is refused, with no partial staging.
 for arch in resources.ARCHES:
  with zipfile.ZipFile(archives/f'asterion-services-linux-{arch}.zip') as archive:
   manifest=json.loads(archive.read('manifest.json'))
   payload={name:archive.read(name) for name in resources.files_for(arch)}
  old=dict(manifest);old['product_version']='0.0.0'
  try: resources.validate(old,payload,arch)
  except resources.BundleNeedsBuild: pass
  else: raise AssertionError('another product version accepted')
  with zipfile.ZipFile(archives/f'asterion-services-linux-{arch}.zip','w') as archive:
   archive.writestr('manifest.json',json.dumps(old))
   for name,data in payload.items(): archive.writestr(name,data)
  rejected=subprocess.run(
   [sys.executable,str(source/'scripts/desktop/remote_resources.py'),'stage',
    '--archives',str(archives),'--directory',str(target)],
   capture_output=True,text=True)
  assert rejected.returncode==1
  assert 'product version differs from this desktop version' in rejected.stderr
  assert 'Traceback' not in rejected.stderr
  resources.verify(target)
  old=dict(manifest);old.pop('os')
  try: resources.validate(old,payload,arch)
  except ValueError: pass
  else: raise AssertionError('incomplete manifest accepted')

 # No partial staging when an architecture is missing.
 (archives/'asterion-services-linux-x86_64.zip').unlink()
 try: resources.stage(archives,root/'incomplete')
 except resources.BundleNeedsBuild: pass
 else: raise AssertionError('missing architecture accepted')
 assert not (root/'incomplete').exists()
 # The native bridge takes its Terminal lifetime lock in the supplied node directory.
 (root/'state').mkdir()
 app=subprocess.Popen([sys.argv[1]],env=dict(os.environ,ASTERION_ROOT=str(root/'state'),ASTERION_REMOTE_RESOURCES=str(target)),stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,text=True,encoding="utf-8")
 def call(path):
  app.stdin.write(json.dumps(dict(version=1,method='node.initializer.export',params=dict(path=str(path))))+'\n');app.stdin.flush()
  return json.loads(app.stdout.readline())
 try:
  expected=(source/'scripts/node/initialize-linux.py').read_text(encoding="utf-8")
  assert call('')['result']['initializer']['content']==expected
  export=root/'initialize-linux.py'
  assert 'result' in call(export) and export.read_text(encoding="utf-8")==expected
  assert 'error' in call(export), 'export overwrote an existing file'
  manifest=target/'x86_64/manifest.json';original=manifest.read_text(encoding="utf-8");data=json.loads(original)
  data['product_version']='wrong';manifest.write_text(json.dumps(data));assert 'error' in call('');manifest.write_text(original)
  data=json.loads(original);data['unexpected']='field';manifest.write_text(json.dumps(data));assert 'error' in call('');manifest.write_text(original)
  # Exporting the initializer does not scan unrelated service programs.
  binary=target/'x86_64/asterion_node_agent'; data=binary.read_bytes();binary.write_bytes(data+b'corruption')
  assert call('')['result']['initializer']['content']==expected
  try: resources.verify(target)
  except ValueError: pass
  else: raise AssertionError('checksum mismatch accepted')
  binary.write_bytes(data)
  # The selected resource must still exist and match its manifest.
  initializer=target/'x86_64/initialize-linux.py';original_initializer=initializer.read_bytes()
  initializer.unlink();assert 'error' in call('')
  initializer.write_bytes(original_initializer+b'corrupt');assert 'error' in call('')
  refused_export=root/'refused.py'
  assert 'error' in call(refused_export)
  assert not refused_export.exists()
  initializer.write_bytes(original_initializer)
  assert 'result' in call('')
  # ZIP path traversal is rejected before extraction.
  with zipfile.ZipFile(archives/'evil.zip','w') as archive: archive.writestr('../escape','test')
  try: resources.read_archive(archives/'evil.zip','x86_64')
  except ValueError: pass
  else: raise AssertionError('unexpected archive member accepted')
 finally:
  app.terminate();app.communicate(timeout=10)
print('Linux x86_64, version/checksum validation, missing payload rejection and native initializer export passed')
