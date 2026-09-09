import {
  decodeUsageResponse,
  normalizeUsage,
  validateWorkspace,
  type UsageFrame,
} from "./usage.ts";

const origin = "https://opencode.ai";
type Fetch = typeof globalThis.fetch;

export class OpenCodeClient {
  private queryId?: string;
  constructor(
    private readonly cookie: string,
    private readonly workspace: string,
    private readonly request: Fetch = globalThis.fetch,
  ) {
    validateWorkspace(workspace);
    if (!/^auth=[^\r\n;]+$/.test(cookie))
      throw new Error("認証情報が不正です。再ログインしてください。");
  }

  private async get(url: string, init: RequestInit = {}): Promise<Response> {
    const response = await this.request(url, {
      ...init,
      redirect: "manual",
      signal: AbortSignal.timeout(20_000),
      headers: { ...init.headers, Cookie: this.cookie },
    });
    if (response.status >= 300 && response.status < 400) {
      throw new Error(
        "認証が切れているか、ワークスペースが移行されています。再ログインしてください。",
      );
    }
    if (!response.ok)
      throw new Error(
        `OpenCodeの取得に失敗しました (HTTP ${response.status})。`,
      );
    if (response.headers.has("X-Error"))
      throw new Error(
        "OpenCodeが取得エラーを返しました。認証と契約状態を確認してください。",
      );
    return response;
  }

  private async discoverQuery(): Promise<string> {
    const response = await this.get(`${origin}/workspace/${this.workspace}/go`);
    const html = await response.text();
    if (html.length > 8_000_000)
      throw new Error("公式ページのサイズが想定を超えました。");
    const assets = [
      ...new Set(
        [...html.matchAll(/\/_build\/assets\/[A-Za-z0-9_-]+\.js/g)].map(
          (match) => match[0],
        ),
      ),
    ];
    if (assets.length === 0 || assets.length > 100)
      throw new Error(
        "公式ページの構成が変わっています。取得処理を更新してください。",
      );
    // Only load first-party modules referenced by the authenticated page. Server
    // function hashes change with deployments and must not be hardcoded.
    const candidates = assets.filter((asset) => /\/index-/.test(asset));
    for (const asset of candidates) {
      const code = await (await this.get(origin + asset)).text();
      const match = code.match(
        /queryLiteSubscription_query\s*=\s*[\w$]+\(\s*["']([a-f0-9]{64})["']\s*\)/,
      );
      if (match?.[1]) return match[1];
    }
    throw new Error(
      "Go使用量の取得先を公式ページから検出できません。取得処理を更新してください。",
    );
  }

  async usage(): Promise<UsageFrame> {
    for (let attempt = 0; attempt < 2; attempt++) {
      this.queryId ??= await this.discoverQuery();
      try {
        const response = await this.get(`${origin}/_server`, {
          method: "POST",
          headers: {
            "Content-Type": "application/json",
            "X-Server-Id": this.queryId,
            "X-Server-Instance": "server-fn:0",
          },
          body: JSON.stringify({
            t: { t: 9, i: 0, l: 1, a: [{ t: 1, s: this.workspace }], o: 0 },
            f: 31,
            m: [],
          }),
        });
        return normalizeUsage(
          decodeUsageResponse(
            await response.text(),
            response.headers.get("Content-Type") ?? "",
          ),
        );
      } catch (error) {
        this.queryId = undefined;
        if (attempt === 1) throw error;
      }
    }
    throw new Error("使用量を取得できませんでした。");
  }

  cachedQueryId(): string | undefined {
    return this.queryId;
  }
}
