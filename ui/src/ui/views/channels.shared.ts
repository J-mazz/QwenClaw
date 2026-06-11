import { html, nothing } from "lit";
import { formatRelativeTimestamp } from "../format.ts";
import type { ChannelAccountSnapshot } from "../types.ts";
import { renderChannelConfigSection } from "./channels.config.ts";
import type { ChannelKey, ChannelsProps } from "./channels.types.ts";

export function channelEnabled(key: ChannelKey, props: ChannelsProps) {
  const snapshot = props.snapshot;
  const channels = snapshot?.channels as Record<string, unknown> | null;
  if (!snapshot || !channels) {
    return false;
  }
  const channelStatus = channels[key] as Record<string, unknown> | undefined;
  const configured = typeof channelStatus?.configured === "boolean" && channelStatus.configured;
  const running = typeof channelStatus?.running === "boolean" && channelStatus.running;
  const connected = typeof channelStatus?.connected === "boolean" && channelStatus.connected;
  const accounts = snapshot.channelAccounts?.[key] ?? [];
  const accountActive = accounts.some(
    (account) => account.configured || account.running || account.connected,
  );
  return configured || running || connected || accountActive;
}

export function getChannelAccountCount(
  key: ChannelKey,
  channelAccounts?: Record<string, ChannelAccountSnapshot[]> | null,
): number {
  return channelAccounts?.[key]?.length ?? 0;
}

export function renderChannelAccountCount(
  key: ChannelKey,
  channelAccounts?: Record<string, ChannelAccountSnapshot[]> | null,
) {
  const count = getChannelAccountCount(key, channelAccounts);
  if (count < 2) {
    return nothing;
  }
  return html`<div class="account-count">Accounts (${count})</div>`;
}

export type ChannelStatusLike = {
  configured?: boolean;
  running?: boolean;
  lastStartAt?: number | null;
  lastProbeAt?: number | null;
  lastError?: string | null;
  probe?: { ok: boolean; status?: number | string | null; error?: string | null } | null;
};

export type ChannelStatusRow = { label: string; value: unknown };

/**
 * Shared status card used by the simple channel views (Discord, Slack,
 * Signal, iMessage, Google Chat). Channel-specific rows can be inserted
 * between "Running" and "Last start" via `extraRows`.
 */
export function renderChannelStatusCard(params: {
  props: ChannelsProps;
  channelId: ChannelKey;
  title: string;
  subtitle: string;
  status?: ChannelStatusLike | null;
  accountCountLabel: unknown;
  extraRows?: ChannelStatusRow[];
  showNAWhenMissing?: boolean;
}) {
  const { props, channelId, title, subtitle, status, accountCountLabel } = params;
  const yesNo = (value: boolean | undefined) =>
    params.showNAWhenMissing && !status ? "n/a" : value ? "Yes" : "No";

  return html`
    <div class="card">
      <div class="card-title">${title}</div>
      <div class="card-sub">${subtitle}</div>
      ${accountCountLabel}

      <div class="status-list" style="margin-top: 16px;">
        <div>
          <span class="label">Configured</span>
          <span>${yesNo(status?.configured)}</span>
        </div>
        <div>
          <span class="label">Running</span>
          <span>${yesNo(status?.running)}</span>
        </div>
        ${(params.extraRows ?? []).map(
          (row) => html`
          <div>
            <span class="label">${row.label}</span>
            <span>${row.value}</span>
          </div>
        `,
        )}
        <div>
          <span class="label">Last start</span>
          <span>${status?.lastStartAt ? formatRelativeTimestamp(status.lastStartAt) : "n/a"}</span>
        </div>
        <div>
          <span class="label">Last probe</span>
          <span>${status?.lastProbeAt ? formatRelativeTimestamp(status.lastProbeAt) : "n/a"}</span>
        </div>
      </div>

      ${
        status?.lastError
          ? html`<div class="callout danger" style="margin-top: 12px;">
            ${status.lastError}
          </div>`
          : nothing
      }

      ${
        status?.probe
          ? html`<div class="callout" style="margin-top: 12px;">
            Probe ${status.probe.ok ? "ok" : "failed"} ·
            ${status.probe.status ?? ""} ${status.probe.error ?? ""}
          </div>`
          : nothing
      }

      ${renderChannelConfigSection({ channelId, props })}

      <div class="row" style="margin-top: 12px;">
        <button class="btn" @click=${() => props.onRefresh(true)}>
          Probe
        </button>
      </div>
    </div>
  `;
}
