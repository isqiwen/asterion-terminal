"""Test requests over Warehouse's public pinned TLS protocol."""
from asterion.warehouse_pb2 import Request, Response
import warehouse_tls


def send(stream, operation, **values):
    request = Request(id=1)
    call = getattr(request, operation)
    call.SetInParent()
    call.CopyFrom(call.__class__(**values))
    payload = request.SerializeToString()
    stream.sendall(len(payload).to_bytes(4, 'big') + payload)


def receive(stream):
    def read(size):
        result = bytearray()
        while len(result) < size:
            chunk = stream.recv(size - len(result))
            assert chunk, 'Warehouse disconnected'
            result.extend(chunk)
        return bytes(result)
    response = Response.FromString(read(int.from_bytes(read(4), 'big')))
    assert not response.HasField('error'), response
    return response


def rpc(tls, port, operation, role='reader', **values):
    """The result of one call, which has the call's name."""
    with warehouse_tls.connect(tls, port, role) as stream:
        send(stream, operation, **values)
        return getattr(receive(stream), operation)
