export interface StoredSession {
  readonly version: 1;
  /** `auth=<value>` 形式の Cookie ヘッダー値。 */
  readonly cookie: string;
  readonly workspace: string;
  /** Unix epoch milliseconds。Cookie に有効期限がある場合だけ設定する。 */
  readonly expiresAt?: number;
}

export interface LoginOptions {
  /** 既存の Firefox Marionette に接続するポート。 */
  readonly port?: number;
  /** ログイン後に開く OpenCode ワークスペース。 */
  readonly workspace?: string;
  /** ログイン完了まで待つ上限時間（ミリ秒）。 */
  readonly timeoutMs?: number;
  /** false の場合は Firefox を起動せず、既存の Marionette へ接続する。 */
  readonly launch?: boolean;
}
