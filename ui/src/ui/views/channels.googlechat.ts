import type { GoogleChatStatus } from "../types.ts";
import { renderChannelStatusCard } from "./channels.shared.ts";
import type { ChannelsProps } from "./channels.types.ts";

export function renderGoogleChatCard(params: {
  props: ChannelsProps;
  googleChat?: GoogleChatStatus | null;
  accountCountLabel: unknown;
}) {
  const { googleChat } = params;
  return renderChannelStatusCard({
    props: params.props,
    channelId: "googlechat",
    title: "Google Chat",
    subtitle: "Chat API webhook status and channel configuration.",
    status: googleChat,
    accountCountLabel: params.accountCountLabel,
    showNAWhenMissing: true,
    extraRows: [
      { label: "Credential", value: googleChat?.credentialSource ?? "n/a" },
      {
        label: "Audience",
        value: googleChat?.audienceType
          ? `${googleChat.audienceType}${googleChat.audience ? ` · ${googleChat.audience}` : ""}`
          : "n/a",
      },
    ],
  });
}
