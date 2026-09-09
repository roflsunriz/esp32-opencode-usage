const maximumFrameLength = 1_000_000;

function protocolError(): Error {
  return new Error(
    "Firefox との認証セッション通信に失敗しました。ログイン画面を確認して再試行してください。",
  );
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return value !== null && typeof value === "object" && !Array.isArray(value);
}

function waitFor<T>(promise: Promise<T>, timeoutMs: number): Promise<T> {
  return new Promise<T>((resolve, reject) => {
    const timer = setTimeout(() => reject(protocolError()), timeoutMs);
    void promise.then(
      (value) => {
        clearTimeout(timer);
        resolve(value);
      },
      () => {
        clearTimeout(timer);
        reject(protocolError());
      },
    );
  });
}

class FrameReader {
  private buffer = Buffer.alloc(0);
  private readonly frames: unknown[] = [];
  private waiter?: {
    resolve(value: unknown): void;
    reject(reason: Error): void;
  };
  private closed = false;

  next(): Promise<unknown> {
    const frame = this.frames.shift();
    if (frame !== undefined) return Promise.resolve(frame);
    if (this.closed) return Promise.reject(protocolError());
    return new Promise<unknown>((resolve, reject) => {
      this.waiter = { resolve, reject };
    });
  }

  accept(chunk: Uint8Array, socket: Bun.Socket): void {
    if (this.closed) return;
    this.buffer = Buffer.concat([this.buffer, Buffer.from(chunk)]);

    try {
      while (true) {
        const colon = this.buffer.indexOf(0x3a);
        if (colon < 0) {
          if (this.buffer.length > 8) throw protocolError();
          return;
        }
        const lengthText = this.buffer.subarray(0, colon).toString("ascii");
        if (!/^[1-9]\d{0,6}$/.test(lengthText)) throw protocolError();
        const length = Number(lengthText);
        if (!Number.isSafeInteger(length) || length > maximumFrameLength)
          throw protocolError();
        const frameEnd = colon + 1 + length;
        if (this.buffer.length < frameEnd) return;

        const payload = this.buffer
          .subarray(colon + 1, frameEnd)
          .toString("utf8");
        this.buffer = this.buffer.subarray(frameEnd);
        this.receive(JSON.parse(payload) as unknown);
      }
    } catch {
      socket.terminate();
      this.fail();
    }
  }

  fail(): void {
    if (this.closed) return;
    this.closed = true;
    const waiter = this.waiter;
    this.waiter = undefined;
    waiter?.reject(protocolError());
  }

  private receive(frame: unknown): void {
    const waiter = this.waiter;
    if (waiter) {
      this.waiter = undefined;
      waiter.resolve(frame);
      return;
    }
    this.frames.push(frame);
  }
}

export interface MarionetteClient {
  command(name: string, parameters?: Record<string, unknown>): Promise<unknown>;
  close(): void;
}

class Protocol3Client implements MarionetteClient {
  private nextMessageId = 0;
  private queue: Promise<void> = Promise.resolve();

  constructor(
    private readonly socket: Bun.Socket,
    private readonly reader: FrameReader,
    private readonly timeoutMs: number,
  ) {}

  command(
    name: string,
    parameters: Record<string, unknown> = {},
  ): Promise<unknown> {
    const request = this.queue.then(() => this.send(name, parameters));
    this.queue = request.then(
      () => undefined,
      () => undefined,
    );
    return request;
  }

  close(): void {
    this.socket.end();
  }

  private async send(
    name: string,
    parameters: Record<string, unknown>,
  ): Promise<unknown> {
    const messageId = ++this.nextMessageId;
    const payload = Buffer.from(
      JSON.stringify([0, messageId, name, parameters]),
      "utf8",
    );
    if (payload.length > maximumFrameLength) throw protocolError();
    const framed = Buffer.concat([
      Buffer.from(`${payload.length}:`, "ascii"),
      payload,
    ]);

    try {
      const written = this.socket.write(framed);
      if (written !== framed.length) throw protocolError();
      this.socket.flush();

      const response = await waitFor(this.reader.next(), this.timeoutMs);
      if (
        !Array.isArray(response) ||
        response.length !== 4 ||
        response[0] !== 1 ||
        response[1] !== messageId
      ) {
        throw protocolError();
      }
      if (response[2] !== null) throw protocolError();
      return response[3];
    } catch {
      this.close();
      throw protocolError();
    }
  }
}

/** localhost 上の Firefox Marionette Protocol 3 にだけ接続する。 */
export async function connectMarionette(
  port: number,
  timeoutMs: number,
): Promise<MarionetteClient> {
  if (
    !Number.isSafeInteger(port) ||
    port < 1 ||
    port > 65535 ||
    !Number.isSafeInteger(timeoutMs) ||
    timeoutMs < 1
  ) {
    throw protocolError();
  }

  const reader = new FrameReader();
  let socket: Bun.Socket | undefined;
  try {
    socket = await waitFor(
      Bun.connect({
        hostname: "127.0.0.1",
        port,
        socket: {
          data(received, chunk) {
            reader.accept(chunk, received);
          },
          end() {
            reader.fail();
          },
          close() {
            reader.fail();
          },
          error() {
            reader.fail();
          },
          connectError() {
            reader.fail();
          },
          timeout() {
            reader.fail();
          },
        },
      }),
      timeoutMs,
    );
    const hello = await waitFor(reader.next(), timeoutMs);
    if (
      !isRecord(hello) ||
      hello.applicationType !== "gecko" ||
      hello.marionetteProtocol !== 3
    ) {
      throw protocolError();
    }
    return new Protocol3Client(socket, reader, timeoutMs);
  } catch {
    socket?.terminate();
    throw protocolError();
  }
}
