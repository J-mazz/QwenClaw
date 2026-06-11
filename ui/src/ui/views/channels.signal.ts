import type { SignalStatus } from "../types.ts";
import { renderChannelStatusCard } from "./channels.shared.ts";
import type { ChannelsProps } from "./channels.types.ts";

export function renderSignalCard(params: {
  props: ChannelsProps;
  signal?: SignalStatus | null;
  accountCountLabel: unknown;
}) {
  return renderChannelStatusCard({
    props: params.props,
    channelId: "signal",
    title: "Signal",
    subtitle: "signal-cli status and channel configuration.",
    status: params.signal,
    accountCountLabel: params.accountCountLabel,
    extraRows: [{ label: "Base URL", value: params.signal?.baseUrl ?? "n/a" }],
  });
}
