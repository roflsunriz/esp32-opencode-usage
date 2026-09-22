import {
  decodeUsageResponse,
  normalizeUsage,
  validateWorkspace,
  type UsageFrame,
} from "./usage.ts";

const origin = "https://opencode.ai";
/** コンソールのセッション Cookie。旧来の auth Cookie では取得できない。 */
const sessionCookiePattern = /^__Host-console_session=[^\r\n;]+$/;
type Fetch = typeof globalThis.fetch;

export class OpenCodeClient {
  constructor(
    private readonly cookie: string,
    private readonly workspace: string,
    private readonly request: Fetch = globalThis.fetch,
  ) {
    validateWorkspace(workspace);
    if (!sessionCookiePattern.test(cookie))
      throw new Error("認証情報が不正です。再ログインしてください。");
  }

  async usage(): Promise<UsageFrame> {
    const response = await this.request(`${origin}/console/api/go/status`, {
      redirect: "manual",
      signal: AbortSignal.timeout(20_000),
      headers: { Cookie: this.cookie, "x-org-id": this.workspace },
    });
    if (response.status === 401 || response.status === 403) {
      throw new Error(
        "認証が切れているか、ワークスペースが移行されています。再ログインしてください。",
      );
    }
    if (response.status >= 300 && response.status < 400) {
      throw new Error(
        "認証が切れているか、ワークスペースが移行されています。再ログインしてください。",
      );
    }
    if (!response.ok)
      throw new Error(
        `OpenCodeの取得に失敗しました (HTTP ${response.status})。`,
      );
    return normalizeUsage(
      decodeUsageResponse(
        await response.text(),
        response.headers.get("Content-Type") ?? "",
      ),
    );
  }
}
