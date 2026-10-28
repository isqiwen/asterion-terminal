// The UI's requests to the Python lab service, forwarded as bytes. The page
// builds and reads the Protobuf messages with the generated types; the Electron
// main process (and the development server) owns the socket and asks the native
// layer where it is, never the page.
const net = require("node:net");

const MAX_REQUEST = 1024 * 1024;
const MAX_REPLY = 64 * 1024 * 1024;
const TIMEOUT_MS = 30000;

function exchange(endpoint, payload) {
  return new Promise((resolve, reject) => {
    const socket = net.connect(endpoint);
    const chunks = [];
    let received = 0;
    const fail = error => {
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
      if (received < 4) return;
      const all = Buffer.concat(chunks);
      const size = all.readUInt32BE(0);
      if (size === 0 || size > MAX_REPLY) return fail(new Error("Invalid lab service reply"));
      if (all.length < 4 + size) return;
      socket.end();
      resolve(all.subarray(4, 4 + size));
    });
    socket.on("end", () => {
      if (received < 4) fail(new Error("Lab service closed the connection"));
    });
  });
}

// `resolve()` returns the endpoint from the native layer; it is asked again
// after a failure, since the Agent may have restarted the service elsewhere.
function labChannel(resolve) {
  let endpoint = null;
  return async payload => {
    const bytes = Buffer.from(payload);
    if (bytes.length === 0 || bytes.length > MAX_REQUEST) throw new Error("Invalid lab request");
    endpoint ??= await resolve();
    try {
      return await exchange(endpoint, bytes);
    } catch (error) {
      endpoint = null;
      throw error;
    }
  };
}

module.exports = { labChannel };
