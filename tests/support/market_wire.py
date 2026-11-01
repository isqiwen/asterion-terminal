"""Test requests over Market's public pinned TLS protocol."""
from asterion.market_pb2 import Request, Response
import market_tls


def send(stream, operation, **values):
    request = Request(version=1, service_id='market-data', correlation_id=operation)
    getattr(request, operation).CopyFrom(getattr(request, operation).__class__(**values))
    payload = request.SerializeToString()
    stream.sendall(len(payload).to_bytes(4, 'big') + payload)


def receive(stream):
    def read(size):
        result = bytearray()
        while len(result) < size:
            chunk = stream.recv(size - len(result))
            assert chunk, 'Market disconnected'
            result.extend(chunk)
        return bytes(result)
    response = Response.FromString(read(int.from_bytes(read(4), 'big')))
    assert not response.HasField('error'), response
    return response


def rpc(tls, port, operation, role='reader', **values):
    with market_tls.connect(tls, port, role) as stream:
        send(stream, operation, **values)
        return receive(stream)
