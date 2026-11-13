"""Test-only certificates; uses the same pinned mutual TLS contract as clients."""
import hashlib
import shutil
import socket
import ssl
import subprocess
from pathlib import Path


def prepare(root):
    root = Path(root)
    tls = root / 'config/tls'
    tls.mkdir(parents=True, exist_ok=True)
    for name in ('server', 'reader', 'manager', 'stranger'):
        if not (tls / (name + '.crt')).exists():
            subprocess.run(['openssl', 'req', '-x509', '-newkey', 'ec', '-pkeyopt',
                'ec_paramgen_curve:prime256v1', '-nodes', '-days', '2', '-subj', '/CN=' + name,
                '-keyout', str(tls / (name + '.key')), '-out', str(tls / (name + '.crt'))],
                check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            (tls / (name + '.key')).chmod(0o600)
    return tls


def fingerprint(tls, name='server'):
    return hashlib.sha256(ssl.PEM_cert_to_DER_cert((tls / (name + '.crt')).read_text())).hexdigest()


def clients(tls):
    return [dict(fingerprint=fingerprint(tls, name)) for name in ('reader', 'manager')]


def connect(tls, port, name='reader', pin=None):
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.minimum_version = ssl.TLSVersion.TLSv1_3
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    context.load_cert_chain(str(tls / (name + '.crt')), str(tls / (name + '.key')))
    connection = socket.create_connection(('127.0.0.1', port), timeout=10)
    try:
        stream = context.wrap_socket(connection)
        assert hashlib.sha256(stream.getpeercert(binary_form=True)).hexdigest() == (pin or fingerprint(tls))
        return stream
    except BaseException:
        connection.close()
        raise


def install_client(tls, node):
    target = Path(node) / 'terminal/data/tls'
    target.mkdir(parents=True, exist_ok=True)
    for extension in ('crt', 'key'):
        shutil.copy2(tls / ('reader.' + extension), target / ('client.' + extension))
