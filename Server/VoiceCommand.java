package net.sf.l2jdev.gameserver.handler.voicedcommandhandlers;

import net.sf.l2jdev.gameserver.handler.IVoicedCommandHandler;
import net.sf.l2jdev.gameserver.model.actor.Player;
import net.sf.l2jdev.gameserver.model.voicechat.VoiceChatManager;
import net.sf.l2jdev.gameserver.network.serverpackets.ExShowScreenMessage;

public class VoiceCommand implements IVoicedCommandHandler
{
	private static final String[] COMMANDS = { "voice" };

	@Override
	public boolean onCommand(String command, Player player, String params)
	{
		VoiceChatManager mgr = VoiceChatManager.getInstance();
		mgr.toggleMute(player.getName());
		boolean muted = mgr.isMuted(player.getName());

		// Mensagem na tela por 3 segundos
		player.sendPacket(new ExShowScreenMessage(muted ? "Chat de voz: DESLIGADO" : "Chat de voz: LIGADO", 3000));
		player.sendMessage(muted ? "Chat de voz desligado. Use .voice para religar." : "Chat de voz ligado. Use .voice para desligar.");
		return true;
	}

	@Override
	public String[] getCommandList()
	{
		return COMMANDS;
	}
}