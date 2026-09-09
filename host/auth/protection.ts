const protectScript = String.raw`
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Security
[Console]::InputEncoding = [System.Text.Encoding]::UTF8
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$plainText = [Console]::In.ReadToEnd()
$plainBytes = [System.Text.Encoding]::UTF8.GetBytes($plainText)
$protectedBytes = [System.Security.Cryptography.ProtectedData]::Protect(
  $plainBytes,
  $null,
  [System.Security.Cryptography.DataProtectionScope]::CurrentUser
)
[Console]::Out.Write([System.Convert]::ToBase64String($protectedBytes))
`;

const unprotectScript = String.raw`
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Security
[Console]::InputEncoding = [System.Text.Encoding]::UTF8
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$protectedText = [Console]::In.ReadToEnd()
$protectedBytes = [System.Convert]::FromBase64String($protectedText)
$plainBytes = [System.Security.Cryptography.ProtectedData]::Unprotect(
  $protectedBytes,
  $null,
  [System.Security.Cryptography.DataProtectionScope]::CurrentUser
)
[Console]::Out.Write([System.Text.Encoding]::UTF8.GetString($plainBytes))
`;

const dpapiTimeoutMs = 15_000;

function unsupportedPlatform(): Error {
  return new Error(
    "認証情報の安全な保存には Windows DPAPI が必要です。このOSでは続行できません。",
  );
}

async function runDpapi(script: string, value: string): Promise<string> {
  if (process.platform !== "win32") throw unsupportedPlatform();

  const child = (() => {
    try {
      return Bun.spawn(
        [
          "powershell.exe",
          "-NoLogo",
          "-NoProfile",
          "-NonInteractive",
          "-Command",
          script,
        ],
        {
          stdin: new TextEncoder().encode(value),
          stdout: "pipe",
          stderr: "ignore",
        },
      );
    } catch {
      throw new Error(
        "Windows の資格情報保護を開始できませんでした。PowerShell の状態を確認してください。",
      );
    }
  })();

  let timer: ReturnType<typeof setTimeout> | undefined;
  try {
    const timeout = new Promise<never>((_resolve, reject) => {
      timer = setTimeout(() => {
        child.kill();
        reject(
          new Error(
            "Windows の資格情報保護が時間内に完了しませんでした。Windows のユーザーセッションを確認してください。",
          ),
        );
      }, dpapiTimeoutMs);
    });
    const [output, exitCode] = await Promise.race([
      Promise.all([new Response(child.stdout).text(), child.exited]),
      timeout,
    ]);
    if (exitCode !== 0 || output.length === 0) {
      throw new Error(
        "Windows の資格情報保護に失敗しました。Windows のユーザーセッションを確認してください。",
      );
    }
    return output;
  } finally {
    if (timer) clearTimeout(timer);
  }
}

/** 現在の Windows ユーザーだけが復号できる DPAPI 文字列を返す。 */
export async function protect(value: string): Promise<string> {
  return runDpapi(protectScript, value);
}

/** `protect` が返した DPAPI 文字列を現在の Windows ユーザーとして復号する。 */
export async function unprotect(value: string): Promise<string> {
  return runDpapi(unprotectScript, value);
}
