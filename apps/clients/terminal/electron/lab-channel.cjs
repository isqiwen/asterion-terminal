// The UI's requests to the Python lab service, forwarded as bytes. The page
// builds and reads the Protobuf messages with the generated types; the Electron
// main process (and the development server) owns the socket and asks the native
// layer where it is, never the page.
const net = require("node:net");

const MAX_REQUEST = 1024 * 1024;
const MAX_REPLY = 64 * 1024 * 1024;
// The longest synchronous Lab operation is a report (60 s including cold
// native imports). Leave time for its explicit timeout reply to arrive.
const TIMEOUT_MS = 65000;

function exchange(endpoint, payload, active) {
  return new Promise((resolve, reject) => {
    const socket = net.connect(endpoint);
    active.add(socket);
    const chunks = [];
    let received = 0;
    let size;
    const fail = error => {
      active.delete(socket);
      socket.destroy();
      reject(error);
    };
    socket.setTimeout(TIMEOUT_MS, () => fail(new Error("Lab service did not reply in time")));
    socket.on("error", fail);
    socket.on("connect", () => {
      const header = Buffer.alloc(4);
      header.writeUInt32BE(payload.length);
      socket.write(Buffer.concat([header, payload]));
    });
    socket.on("data", chunk => {
      chunks.push(chunk);
      received += chunk.length;
      if (size === undefined) {
        if (received < 4) return;
        size = Buffer.concat(chunks).readUInt32BE(0);
        if (size === 0 || size > MAX_REPLY) return fail(new Error("Invalid lab service reply"));
      }
      if (received < 4 + size) return;
      active.delete(socket);
      socket.end();
      resolve(Buffer.concat(chunks).subarray(4, 4 + size));
    });
    // After a complete reply this rejects nothing; before it, the reply was cut off.
    socket.on("close", () => {
      active.delete(socket);
      fail(new Error("Lab service closed the connection"));
    });
  });
}

// `resolve()` returns the endpoint from the native layer; it is asked again
// after a failure, since the Agent may have restarted the service elsewhere.
function labChannel(resolve) {
  let endpoint = null;
  let resolving = null;
  let closed = false;
  const active = new Set();
  const send = async payload => {
    if (closed) throw new Error("Lab channel is closing");
    const bytes = Buffer.from(payload);
    if (bytes.length === 0 || bytes.length > MAX_REQUEST) throw new Error("Invalid lab request");
    const used = await (endpoint
      ? Promise.resolve(endpoint)
      : (resolving ??= resolve().then(
          found => {
            endpoint = found;
            resolving = null;
            return found;
          },
          error => {
            resolving = null;
            throw error;
          },
        )));
    if (closed) throw new Error("Lab channel is closing");
    try {
      return await exchange(used, bytes, active);
    } catch (error) {
      if (endpoint === used) endpoint = null;
      throw error;
    }
  };
  send.close = () => {
    closed = true;
    for (const socket of active) socket.destroy();
    active.clear();
  };
  return send;
}

module.exports = { labChannel };
