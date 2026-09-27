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

source=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('resources',source/'scripts/remote_resources.py')
resources=importlib.util.module_from_spec(spec);spec.loader.exec_module(resources)
with tempfile.TemporaryDirectory() as folder:
 root=Path(folder); fixture=make_bundle(root/'fixture'); archives=root/'archives'; archives.mkdir()
 for arch in resources.ARCHES:
  with zipfile.ZipFile(archives/f'asterion-services-linux-{arch}.zip','w') as archive:
   for file in (fixture/arch).iterdir(): archive.write(file,file.name)
 try: resources.files_for('arm64')
 except ValueError: pass
 else: raise AssertionError('unsupported Linux ARM64 accepted')
 target=root/'staged'; resources.stage(archives,target); resources.verify(target)
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
  binary=target/'x86_64/asterion-node-agent'; data=binary.read_bytes();binary.write_bytes(data+b'corruption');assert 'error' in call('')
  try: resources.verify(target)
  except ValueError: pass
  else: raise AssertionError('checksum mismatch accepted')
  binary.write_bytes(data)
  # Research and strategy executables are mandatory and independently integrity checked.
  for name in ('asterion-task-service', 'asterion-backtest', 'asterion-factor', 'asterion-data-pipeline', 'asterion-strategy'):
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
