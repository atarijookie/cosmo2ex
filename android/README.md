# CE Floppy - Android app

Switches floppy images from a phone, in the app or with a home screen widget. Talks only to the webserver (`webserver/app.go`).

    android/build.sh      # or: make android

Needs only docker, no Android Studio. The result is `webserver/static/app/ce_floppy.apk`, not kept in git; without it
the webserver's page says the app is not included in the build. Building the builder image accepts the Android SDK licenses.

## How it works

- **Pairing:** Settings -> Android app on the webserver shows a QR code, or a link to tap on the phone itself:
  `cosmosex://pair?a=<addresses>&p=<port>&t=<token>&f=<SHA-256 of the local CA>`. The new settings are stored only after
  they have worked. IP or token changed? Scan again.
- **Trust:** the app accepts only certificate chains leading to that CA (`tls.go`, which sends the CA in the chain for this).
  The CA isn't installed on the phone, and the token is sent nowhere else.
- **Token:** `SETTINGS_DIR/app_token`, one for all phones, replaced by "Create a new code". It's valid only for `/app/v1/*`,
  which can browse folders anywhere like the web UI's file selector, but sees only folder and floppy image names, and can only insert floppy images.
- **Prev / next** move a selection stored on the phone, **Insert** sends it - like the buttons on the device, so going
  through a list doesn't load every image. When the image in the drive changes elsewhere, the selection follows it.
- **Updates of what's shown:** the app polls every 3 s while it's open. There's no background service, so the widget refreshes when
  it's tapped, when the app is open, and when Android asks (about every 30 minutes) - its name can be old until then.

## Signing key

`ce_app.keystore` is in git on purpose, with the password in `app/build.gradle.kts`. Android requires a signature and
installs an update only with the same key, this way any build can update any other. It isn't a secret: it can't do
anything except sign an update, which someone would still have to install by hand. If it's ever replaced: uninstall the
app, install the new one, and scan the QR code.

## Releases

The app is part of every release: `release/build.sh` and `docker/build_all.sh` build it (the workflows build it once and hand
it over, `ANDROID_APK` in `release/build.sh`), and the GitHub release has the APK attached too. Plain `docker build` and `docker compose up --build`
take what's in `webserver/static/app/`, so run this first to have the app in there.

`versionCode` stays at 1 on purpose: Android refuses to install only a lower one, so any build can replace any other, and
downgrading works. `versionName` comes from `VERSION`.
