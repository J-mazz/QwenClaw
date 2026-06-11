import type { IMessageStatus } from "../types.ts";
import { renderChannelStatusCard } from "./channels.shared.ts";
import type { ChannelsProps } from "./channels.types.ts";

export function renderIMessageCard(params: {
  props: ChannelsProps;
  imessage?: IMessageStatus | null;
  accountCountLabel: unknown;
}) {
  return renderChannelStatusCard({
    props: params.props,
    channelId: "imessage",
    title: "iMessage",
    subtitle: "macOS bridge status and channel configuration.",
    status: params.imessage,
    accountCountLabel: params.accountCountLabel,
  });
}
