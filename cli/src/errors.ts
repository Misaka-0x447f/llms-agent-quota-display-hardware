export class OAuthError extends Error {
  override readonly name = "OAuthError";
}

export class DeviceError extends Error {
  override readonly name = "DeviceError";
}

export class CliError extends Error {
  override readonly name = "CliError";
}

export class AbortError extends Error {
  override readonly name = "AbortError";
}
