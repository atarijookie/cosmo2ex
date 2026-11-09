# Builder image for the Android floppy app: JDK + Gradle + Android SDK command line tools, no Android Studio. No sources
# inside - those are mounted at run time, see android/build.sh. Building this image accepts the Android SDK licenses.

FROM debian:bookworm-slim

ARG CMDLINE_TOOLS_VERSION=11076708
ARG GRADLE_VERSION=8.10.2
ARG ANDROID_PLATFORM=android-35
ARG BUILD_TOOLS_VERSION=35.0.0

RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl unzip openjdk-17-jdk-headless \
 && rm -rf /var/lib/apt/lists/*

ENV ANDROID_HOME=/opt/android-sdk
ENV PATH=/opt/gradle/bin:/opt/android-sdk/cmdline-tools/latest/bin:$PATH

RUN curl -fsSL -o /tmp/gradle.zip "https://services.gradle.org/distributions/gradle-${GRADLE_VERSION}-bin.zip" \
 && unzip -q /tmp/gradle.zip -d /opt && mv "/opt/gradle-${GRADLE_VERSION}" /opt/gradle && rm /tmp/gradle.zip

RUN curl -fsSL -o /tmp/tools.zip "https://dl.google.com/android/repository/commandlinetools-linux-${CMDLINE_TOOLS_VERSION}_latest.zip" \
 && mkdir -p "$ANDROID_HOME/cmdline-tools" && unzip -q /tmp/tools.zip -d "$ANDROID_HOME/cmdline-tools" \
 && mv "$ANDROID_HOME/cmdline-tools/cmdline-tools" "$ANDROID_HOME/cmdline-tools/latest" && rm /tmp/tools.zip \
 && (yes | sdkmanager --licenses > /dev/null) \
 && sdkmanager --install "platforms;${ANDROID_PLATFORM}" "build-tools;${BUILD_TOOLS_VERSION}" > /dev/null \
 && chmod -R a+rX "$ANDROID_HOME"
