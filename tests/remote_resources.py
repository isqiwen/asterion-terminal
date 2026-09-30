"""Bundle integrity and native export tests using explicitly synthetic test ELF files."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile
from bundle_fixture import make_bundle
from service_fingerprint import fingerprint, FILES
import shutil

source=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('resources',source/'scripts/remote_resources.py')
resources=importlib.util.module_from_spec(spec);spec.loader.exec_module(resources)
# Fingerprints follow source contents, not checkout location or client UI changes.
with tempfile.TemporaryDirectory() as folder:
 root=Path(folder)/"source"; root.mkdir()
 for name in FILES:
  path=root/name; path.parent.mkdir(parents=True,exist_ok=True); path.write_text("build fixture")
 (root/'protocol').mkdir(); protocol=root/'protocol/model.proto';protocol.write_text('schema 1')
 before=fingerprint(root)
 copy=Path(folder)/'copy';shutil.copytree(root,copy);assert fingerprint(copy)==before
 ui=root/'apps/clients/terminal/src/view.tsx';ui.parent.mkdir(parents=True);ui.write_text('client UI')
 assert fingerprint(root)==before
 protocol.write_text('schema 2');assert fingerprint(root)!=before
 protocol.write_text('schema 1');assert fingerprint(root)==before
 extra=root/'protocol/extra.proto';extra.write_text('new schema');assert fingerprint(root)!=before
 extra.unlink();assert fingerprint(root)==before

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
 # A matching semantic version and valid file hashes do not permit older sources.
 for arch in resources.ARCHES:
  with zipfile.ZipFile(archives/f'asterion-services-linux-{arch}.zip') as archive:
   manifest=json.loads(archive.read('manifest.json'))
   payload={name:archive.read(name) for name in resources.files_for(arch)}
  old=dict(manifest);old['source_sha256']='0'*64
  try: resources.validate(old,payload,arch)
  except ValueError: pass
  else: raise AssertionError('same-version stale source accepted')
  old=dict(manifest);old['version']=1;old.pop('source_sha256')
  try: resources.validate(old,payload,arch)
  except ValueError: pass
  else: raise AssertionError('old manifest contract accepted')

 # No partial staging when an architecture is missing.
 (archives/'asterion-services-linux-x86_64.zip').unlink()
 try: resources.stage(archives,root/'incomplete')
 except FileNotFoundError: pass
 else: raise AssertionError('missing architecture accepted')
 assert not (root/'incomplete').exists()
 app=subprocess.Popen([sys.argv[1]],env=dict(os.environ,ASTERION_NODE_DIRECTORY=str(root/'state'),ASTERION_REMOTE_RESOURCES=str(target)),stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,text=True,encoding="utf-8")
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
  data=json.loads(original);data['source_sha256']='0'*64;manifest.write_text(json.dumps(data));assert 'error' in call('');manifest.write_text(original)
  binary=target/'x86_64/asterion-node-agent'; data=binary.read_bytes();binary.write_bytes(data+b'corruption');assert 'error' in call('')
  try: resources.verify(target)
  except ValueError: pass
  else: raise AssertionError('checksum mismatch accepted')
  binary.write_bytes(data)
  # Research and strategy executables are mandatory and independently integrity checked.
  for name in ('asterion-task-service', 'asterion-backtest', 'asterion-factor', 'asterion-data-pipeline', 'asterion-strategy', 'plugins/asterion-tushare.so', 'plugins/asterion-order-limits.so'):
   payload=target/'x86_64'/name; original_payload=payload.read_bytes()
   payload.unlink()
   assert 'error' in call(''), name
   payload.write_bytes(original_payload+b'corrupt')
   assert 'error' in call(''), name
   payload.write_bytes(original_payload)
  assert 'result' in call('')
  # ZIP path traversal is rejected before extraction.
  with zipfile.ZipFile(archives/'evil.zip','w') as archive: archive.writestr('../escape','test')
  try: resources.read_archive(archives/'evil.zip','x86_64')
  except ValueError: pass
  else: raise AssertionError('unexpected archive member accepted')
 finally:
  app.terminate();app.communicate(timeout=10)
print('Linux x86_64, version/checksum validation, missing payload rejection and native initializer export passed')
