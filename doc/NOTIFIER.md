nchat-notify
============

`nchat-notify` is a headless daemon built from the nchat code base. It stays
connected to the configured Telegram / WhatsApp / Signal accounts and, when a
new incoming message arrives in one of a configured set of chats, publishes a
preconfigured notification to an [ntfy](https://ntfy.sh) topic.

It reuses `src/main.cpp` and the protocol libraries unchanged, replacing the
ncurses user interface with `notifier/ui.cpp`. It never sends messages, read
receipts or online status, so the accounts behave as if no client was open.


Build
-----

The target is enabled by default (`-DHAS_NOTIFIER=OFF` disables it) and does
not need ncurses at runtime. On a server, typically:

    mkdir -p build && cd build
    cmake -DCMAKE_BUILD_TYPE=Release -DHAS_SIGNAL=OFF -DHAS_TESTS=OFF ..
    make -j4 nchat-notify tgchat wmchat
    sudo make install

The protocol libraries are loaded from `<prefix>/bin/../lib`, so install the
binary and the libraries under the same prefix. Building tdlib needs several
GB of RAM; on a small VPS build on another machine with the same distribution.

Runtime dependency: the `curl` command line tool, used to publish to ntfy.


Account setup
-------------

Account setup is interactive and done once, over ssh, as the service user:

    sudo -u nchat nchat-notify --confdir /var/lib/nchat-notify --setup

For WhatsApp on a server without a graphical display, first set
`use_qr_terminal=1` (QR code drawn in the terminal) or `use_pairing_code=1`
(pairing code entered on the phone) in `/var/lib/nchat-notify/app.conf`.

Recommended `app.conf` settings for a server:

    attachment_prefetch=0
    downloads_dir=/var/lib/nchat-notify/downloads

Only one process may use a confdir at a time, so stop the service while
running setup.


Configuration
-------------

`notifier.conf` in the confdir is created with defaults on first start:

| Parameter             | Default           | Description                                         |
|-----------------------|-------------------|-----------------------------------------------------|
| `ntfy_server`         | `https://ntfy.sh` | ntfy server base url                                |
| `ntfy_topic`          | (empty)           | topic for message notifications, required           |
| `ntfy_token`          | (empty)           | access token, sent as `Authorization: Bearer`       |
| `ntfy_timeout_sec`    | `15`              | timeout per publish request                         |
| `notify_title`        | `%chat%`          | notification title template                         |
| `notify_message`      | `New message`     | notification body template                          |
| `notify_priority`     | `3`               | ntfy priority 1 (min) - 5 (max)                     |
| `notify_tags`         | `speech_balloon`  | comma-separated ntfy tags / emoji shortcodes        |
| `notify_click`        | (empty)           | url opened when the notification is tapped          |
| `notify_cooldown_sec` | `60`              | min interval between notifications for one chat     |
| `watch_chats`         | (empty)           | comma-separated chats to watch, see below           |
| `catchup_max_age_sec` | `86400`           | max age of messages missed while stopped to notify  |
| `status_notify`       | `1`               | publish login failures and session loss             |
| `status_topic`        | (empty)           | topic for status notifications, default `ntfy_topic`|

Templates support the placeholders `%chat%` (chat name), `%chatid%`,
`%sender%` (sender name), `%text%` (message text, truncated), `%profile%` and
`%protocol%`. Leave `%text%` out to avoid sending message content to the ntfy
server.

`watch_chats` entries are `profileid:chatid`, a bare `chatid` (any profile) or
`*` (all chats). Chat ids are protocol internal, so the daemon writes every
chat it sees, with its name, to `notifier_chats.txt` in the confdir: start the
daemon once, then copy the wanted ids from that file. Restart the service after
changing `notifier.conf`.


Behavior
--------

A message triggers a notification if it is incoming, not an edit, not from the
local cache, in a watched chat, and newer than the last notified message of
that chat. The newest notified message time per chat is persisted in
`notifier_state.txt`, so after a restart messages that arrived while the daemon
was stopped are notified (at most `catchup_max_age_sec` back), while the
history fetched at login is not. For a chat without saved state only messages
received after start are notified. Several new messages received within
`notify_cooldown_sec` result in a single notification.

Publishing happens on a background thread, with up to three attempts per
notification. Request data, including the token, is passed to curl on stdin and
never appears on its command line.

If a protocol reports that the session was closed (e.g. the device was unlinked
from the phone), a status notification is published and the daemon exits with
status 0, so systemd does not restart it until setup has been redone.

Logs go to stderr (the journal under systemd: `journalctl -u nchat-notify`) and
to `log.txt` in the confdir.


Service
-------

See `notifier/nchat-notify.service` for a hardened systemd unit and install
instructions.
