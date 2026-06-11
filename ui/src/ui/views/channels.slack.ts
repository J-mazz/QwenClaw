import type { SlackStatus } from "../types.ts";
import { renderChannelStatusCard } from "./channels.shared.ts";
import type { ChannelsProps } from "./channels.types.ts";

export function renderSlackCard(params: {
  props: ChannelsProps;
  slack?: SlackStatus | null;
  accountCountLabel: unknown;
}) {
  return renderChannelStatusCard({
    props: params.props,
    channelId: "slack",
    title: "Slack",
    subtitle: "Socket mode status and channel configuration.",
    status: params.slack,
    accountCountLabel: params.accountCountLabel,
  });
}
