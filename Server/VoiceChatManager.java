package net.sf.l2jdev.gameserver.model.voicechat;

import java.io.IOException;
import java.io.OutputStream;
import java.io.PrintWriter;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.util.List;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.logging.Logger;

import net.sf.l2jdev.commons.threads.ThreadPool;
import net.sf.l2jdev.gameserver.model.World;
import net.sf.l2jdev.gameserver.model.actor.Player;

public class VoiceChatManager
{
	private static final Logger LOGGER = Logger.getLogger(VoiceChatManager.class.getName());

	private static final int PORT = 7778;
	private static final int BROADCAST_INTERVAL_MS = 500;
	private static final int VOICE_RANGE = 2000; // L2 units — ligeiramente maior que range de party

	private final Set<String> _mutedPlayers = ConcurrentHashMap.newKeySet();
	private final List<ClientSession> _sessions = new CopyOnWriteArrayList<>();
	private ServerSocket _serverSocket;

	public static VoiceChatManager getInstance()
	{
		return SingletonHolder.INSTANCE;
	}

	public void start()
	{
		try
		{
			_serverSocket = new ServerSocket(PORT, 50, InetAddress.getByName("0.0.0.0"));
			ThreadPool.execute(this::acceptLoop);
			ThreadPool.scheduleAtFixedRate(this::broadcast, 0, BROADCAST_INTERVAL_MS);
			LOGGER.info("VoiceChatManager: Listening on port " + PORT);
		}
		catch (IOException e)
		{
			LOGGER.warning("VoiceChatManager: Failed to start — " + e.getMessage());
		}
	}

	private void acceptLoop()
	{
		while (!_serverSocket.isClosed())
		{
			try
			{
				Socket socket = _serverSocket.accept();
				ClientSession session = new ClientSession(socket);
				_sessions.add(session);
				LOGGER.fine("VoiceChatManager: Client connected from " + socket.getInetAddress().getHostAddress());
			}
			catch (IOException e)
			{
				if (!_serverSocket.isClosed())
					LOGGER.warning("VoiceChatManager: Accept error — " + e.getMessage());
			}
		}
	}

	private void broadcast()
	{
		_sessions.removeIf(s -> !s.isAlive());

		for (ClientSession session : _sessions)
		{
			Player player = resolvePlayer(session);
			if (player == null)
				continue;
			if (_mutedPlayers.contains(player.getName()))
				continue;

			List<Player> nearby = World.getInstance().getVisibleObjectsInRange(player, Player.class, VOICE_RANGE);

			StringBuilder sb = new StringBuilder();
			sb.append("SELF:").append(player.getX()).append(',').append(player.getY()).append(',').append(player.getZ()).append('\n');

			sb.append("NEARBY:");
			boolean first = true;
			for (Player p : nearby)
			{
				if (p == player || _mutedPlayers.contains(p.getName()))
					continue;
				if (!first)
					sb.append(';');
				sb.append(p.getName()).append(':').append(p.getX()).append(',').append(p.getY()).append(',').append(p.getZ());
				first = false;
			}
			sb.append('\n');

			session.send(sb.toString());
		}
	}

	private Player resolvePlayer(ClientSession session)
	{
		String ip = session.getIp();
		return World.getInstance().getPlayers().stream()
			.filter(p -> p.getClient() != null && ip.equals(p.getClient().getIpAddress()))
			.findFirst()
			.orElse(null);
	}

	public void toggleMute(String playerName)
	{
		if (_mutedPlayers.contains(playerName))
			_mutedPlayers.remove(playerName);
		else
			_mutedPlayers.add(playerName);
	}

	public boolean isMuted(String playerName)
	{
		return _mutedPlayers.contains(playerName);
	}

	private static class ClientSession
	{
		private final Socket _socket;
		private final PrintWriter _writer;
		private final String _ip;

		ClientSession(Socket socket) throws IOException
		{
			_socket = socket;
			_ip = socket.getInetAddress().getHostAddress();
			OutputStream os = socket.getOutputStream();
			_writer = new PrintWriter(os, true);
		}

		void send(String data)
		{
			try
			{
				_writer.print(data);
				_writer.flush();
			}
			catch (Exception e)
			{
				close();
			}
		}

		boolean isAlive()
		{
			return _socket.isConnected() && !_socket.isClosed();
		}

		void close()
		{
			try { _socket.close(); } catch (IOException ignored) {}
		}

		String getIp()
		{
			return _ip;
		}
	}

	private static class SingletonHolder
	{
		static final VoiceChatManager INSTANCE = new VoiceChatManager();
	}
}