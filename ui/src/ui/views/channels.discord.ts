import type { DiscordStatus } from "../types.ts";
import { renderChannelStatusCard } from "./channels.shared.ts";
import type { ChannelsProps } from "./channels.types.ts";

export function renderDiscordCard(params: {
  props: ChannelsProps;
  discord?: DiscordStatus | null;
  accountCountLabel: unknown;
}) {
  return renderChannelStatusCard({
    props: params.props,
    channelId: "discord",
    title: "Discord",
    subtitle: "Bot status and channel configuration.",
    status: params.discord,
    accountCountLabel: params.accountCountLabel,
  });
}
