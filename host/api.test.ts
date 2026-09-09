import { describe, expect, test } from "bun:test";
import { OpenCodeClient } from "./api.ts";

const workspace = "wrk_0123456789ABCDEF";
const query = "a".repeat(64);
const sample = Object.fromEntries(
  ["rolling", "weekly", "monthly"].map((key) => [
    `${key}Usage`,
    {
      usage: 100000000,
      limit: 1000000000,
      usagePercent: 10,
      resetInSec: 18000,
    },
  ]),
);

describe("OpenCode transport", () => {
  test("discovers the deployed query, sends typed args, and caches within a session", async () => {
    const calls: { url: string; init?: RequestInit }[] = [];
    const fake = (async (input: string | URL | Request, init?: RequestInit) => {
      const url = String(input);
      calls.push({ url, init });
      if (url.endsWith("/go"))
        return new Response(
          '<link href="/_build/assets/index-deployed.js"><script src="https://evil.invalid/index-bad.js"></script>',
        );
      if (url.endsWith(".js"))
        return new Response(
          `const queryLiteSubscription_query = createServerReference("${query}");`,
        );
      return Response.json(sample);
    }) as typeof fetch;
    const client = new OpenCodeClient("auth=test", workspace, fake);
    expect((await client.usage()).rolling.used).toBe(1);
    await client.usage();
    expect(calls.map((call) => call.url)).toEqual([
      `https://opencode.ai/workspace/${workspace}/go`,
      "https://opencode.ai/_build/assets/index-deployed.js",
      "https://opencode.ai/_server",
      "https://opencode.ai/_server",
    ]);
    const init = calls[2]?.init;
    expect(init?.redirect).toBe("manual");
    expect(new Headers(init?.headers).get("X-Server-Id")).toBe(query);
    expect(new Headers(init?.headers).get("Cookie")).toBe("auth=test");
    expect(JSON.parse(String(init?.body)).t.a[0].s).toBe(workspace);
  });

  test("refreshes a changed deployment once and does not retry indefinitely", async () => {
    let assetCount = 0;
    let queryCount = 0;
    const fake = (async (input: string | URL | Request) => {
      const url = String(input);
      if (url.endsWith("/go"))
        return new Response('<link href="/_build/assets/index-current.js">');
      if (url.endsWith(".js")) {
        assetCount++;
        return new Response(
          `const queryLiteSubscription_query = f("${query}");`,
        );
      }
      queryCount++;
      return queryCount === 1
        ? new Response("not found", { status: 404 })
        : Response.json(sample);
    }) as typeof fetch;
    expect(
      (await new OpenCodeClient("auth=test", workspace, fake).usage()).monthly
        .limit,
    ).toBe(10);
    expect(assetCount).toBe(2);
    expect(queryCount).toBe(2);
  });

  test("does not follow authentication redirects or forward credentials elsewhere", async () => {
    let count = 0;
    const fake = (async () => {
      count++;
      return new Response(null, {
        status: 302,
        headers: { Location: "https://other.invalid/" },
      });
    }) as unknown as typeof fetch;
    await expect(
      new OpenCodeClient("auth=test", workspace, fake).usage(),
    ).rejects.toThrow("再ログイン");
    expect(count).toBe(1);
    expect(
      () => new OpenCodeClient("auth=test\r\nX-Bad: 1", workspace, fake),
    ).toThrow();
  });

  test("fails visibly for missing Go data instead of treating it as zero", async () => {
    const fake = (async (input: string | URL | Request) => {
      const url = String(input);
      if (url.endsWith("/go"))
        return new Response('<link href="/_build/assets/index-current.js">');
      if (url.endsWith(".js"))
        return new Response(
          `const queryLiteSubscription_query = f("${query}");`,
        );
      return Response.json(null);
    }) as typeof fetch;
    await expect(
      new OpenCodeClient("auth=test", workspace, fake).usage(),
    ).rejects.toThrow();
  });
});
