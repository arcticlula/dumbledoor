# Remote access

At home, the app talks to Dumbledoor directly over the Wi-Fi. To open the door from anywhere (from the street, where the Wi-Fi doesn't reach, or from work to let someone in), Dumbledoor also keeps an outgoing connection to an **MQTT broker** on the internet, and phones away from home connect to the same broker. Nothing has to reach into the home network: no port forwarding, no fixed IP.

The broker only relays messages. Commands are signed with each phone's key (see [protocol.md](protocol.md)), so the broker can't open the door, replay a command or change anything. Every phone has the broker login, so the state everyone can read there only has what opening the door needs. Each admin gets the full state (rings, the activity) encrypted with their own key, and the lobby code travels encrypted too. So neither the broker nor the phones from ordinary links can read the activity. What they can see is when messages go by, so roughly when someone rings or opens. With the lobby code on, presses only go out as one outcome once they stop, so that timing doesn't give the code away.

Away from home, the online app works as at home: a phone from an ordinary link gets the open button, and admins also get rings, the activity, auto-open, mute and the lobby code. Phones, remote access and the other settings only change at home.

It takes four steps, all done once.

## 1. Get a broker

Any MQTT 3.1.1 broker with TLS and WebSockets works. Two good options:

- **EMQX Cloud Serverless** ([emqx.com](https://www.emqx.com/en/cloud/serverless-mqtt)). Free under 1 million session-minutes a month; Dumbledoor's connection uses ~44,000 and each phone only connects while the app is open.
  1. Create a Serverless deployment.
  2. In the deployment's console, add two username/password pairs for authentication: one for Dumbledoor and one for the phones. Anonymous connections aren't allowed.
  3. From the deployment's overview, note its address (something like `x1y2z3.ala.eu-central-1.emqxsl.com`).
  4. The two broker addresses are then:
     - for Dumbledoor: `mqtts://<address>:8883`
     - for phones: `wss://<address>:8084/mqtt`

  EMQX's certificate is signed by DigiCert, which the firmware already trusts.
- **Your own Mosquitto**, on a Raspberry Pi or a small server, with a TLS certificate (Let's Encrypt) and a WebSocket listener. Use `mqtts://host:8883` and `wss://host:<port>/` (with whatever path the WebSocket listener is on).

HiveMQ Cloud's free Serverless plan no longer takes new clusters and shuts down on 31 Dec 2026; its paid Starter plan works the same way (ports 8883 and 8884, path `/mqtt`).

## 2. Put the app online

Away from home, phones load the app from an `https://` address: browsers don't let an `https://` page talk to a plain-`http://` device, and installing the app (Add to Home screen, notifications) needs `https://` anyway. The app is plain static files and holds no secrets; each phone's key arrives in its pairing link.

- **GitHub Pages.** Push this repository to GitHub, then in the repository's Settings → Pages, set Source to "GitHub Actions". The workflow in `.github/workflows/pages.yml` publishes `app/` on every push that changes it, at `https://<user>.github.io/<repository>/`. GitHub Pages needs a public repository on a free account. This repository's copy is at **https://arcticlula.github.io/dumbledoor/** (set up 2026-10-18).
- **Your own site.** Copy `app/` anywhere that serves `https://`. For example, `electrolula/public/dumbledoor/` would make it `https://lulalab.pt/dumbledoor/`.

## 3. Tell Dumbledoor

At home, open `http://dumbledoor.local` with an admin phone, then Settings → **Remote access**:

| Field | Value |
|---|---|
| Use remote access | on |
| Broker, for Dumbledoor | `mqtts://<address>:8883`, its user name and password |
| Broker, for phones | `wss://<address>:8084/mqtt` (required: without it, links only work at home), the phones' user name and password (empty: the same as Dumbledoor's) |
| Topic prefix | leave `dumbledoor`, or anything else |
| Where the online app is | the address from step 2, e.g. `https://arcticlula.github.io/dumbledoor/` |

Save. A few seconds later the line under the form says "Dumbledoor is connected to the broker". If it doesn't, see the troubleshooting section below.

## 4. Pair phones for the online app

Settings → **Phones** → type a name → **Create a pairing link**. With remote access set up, there are now two links: "For the online app", which works anywhere, and one for home Wi-Fi only. Send the first one to the person (or tap "Open here" for this phone), open it, and add the page to the home screen. On Android, Chrome's menu → "Add to Home screen" (or "Install app").

Links made **before** remote access was set up only work at home; make new ones. The same goes after changing the phones' broker password: links carry it, so old links stop connecting.

## Notifications for rings

The app vibrates and shows a banner when someone rings while it's open. With **Notify me when someone rings** on (online app only), it also notifies while it's in the background, as long as the browser keeps it alive. To be told about every ring even with the app closed, use [ntfy](https://ntfy.sh):

1. Install the ntfy app (Android or iOS) and subscribe to a long, random topic, e.g. `dumbledoor-7f3k9q2m4x`.
2. In Dumbledoor's Settings → **Ring notifications**, enter `https://ntfy.sh/dumbledoor-7f3k9q2m4x` and save.
3. Every ring now sends "Someone is ringing at the lobby", at most one every 30 s per line. Tapping it opens the online app, if its address is set.

Topics on ntfy.sh are public: anyone who guesses the name sees the ring notifications (nothing else). Use a long random name, or run your own ntfy server and put its address in instead.

## Troubleshooting

- **Settings says Dumbledoor isn't connected to the broker.** Check the address starts with `mqtts://` and ends in `:8883`, and the user name and password. With USB connected, `idf.py monitor` shows why: "the broker refused the connection (code 4)" means a wrong user name or password, and "can't reach the broker" means a wrong address, or no internet.
- **The online app says "This phone's link has no broker details".** Dumbledoor's remote access wasn't complete when the link was made: **Use remote access** was off, or **Broker, for phones** was empty. Since 2026-10-18 the firmware only offers the online-app link once both are set, and **Where the online app is** too (older firmware offered it as soon as that last one was filled in). Saving Remote access and making a link both say which of the three is still missing. Fix it, wait for "Dumbledoor is connected to the broker", then make a new link.
- **The online app says "Not on the broker".** The phone reached the broker, but Dumbledoor isn't connected to it (see above), or it's using a different topic prefix than the link.
- **The online app says "Can't reach the broker: wrong user name or password".** The link has old credentials: make a new link.
- **It works at home but not on mobile data.** Make sure the phone opened the link for the online app (an `https://` address), not the home one.
