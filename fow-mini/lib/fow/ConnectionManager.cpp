/*
    Copyright (c) 2017, 2018 Declan Freeman-Gleason.

    This file is part of Ferries Over Winslow.

    Ferries Over Winslow is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Ferries Over Winslow is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this Ferries Over Winslow.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "ConnectionManager.h"

ConnectionManager::ConnectionManager(const String programName) : name(programName) {
  WiFi.disconnect();

  // Tell the http client to allow reuse if the server supports it (we make lots of requests to the same server, this should decrease overhead)
  http.setReuse(true);

  Serial.println("Checking EEPROM for saved WiFi credentials...");
  settingsManager.updateFullResetTimer();

  if (!settingsManager.isInSetupMode()) {
    setupMode = false;

    ssid = settingsManager.getSetting(SettingsManager::Setting::SSID);
    password = settingsManager.getSetting(SettingsManager::Setting::PASSWORD);

    Serial.printf("Saved credentials found. SSID: %s, Password: %s.\n", ssid.c_str(), password.c_str());

    WiFi.softAPdisconnect(true); // Make sure that we don't broadcast
    connectToWiFiNetwork();

    return;
  }
  setupMode = true;

  Serial.println("No saved credentials found. Starting the ferry connection configuration WiFi AP...");

  const uint32_t chipIdUint = ESP.getChipId();
  char chipId[sizeof(chipIdUint)] = {0};
  sprintf(chipId, "%06X", chipIdUint);

  // Set a user agent so we can get some device info on the serverside if we ever want to
  http.setUserAgent(name + "/" + VERSION + "/" + chipId);

  // Setup in soft access point mode
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(name + "-" + chipId);
  IPAddress deviceIP = WiFi.softAPIP();

  // Set up a captive portal
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(dnsPort, "*", deviceIP);
  
  // In case the MDNS responder can't start
  Serial.printf("Server local IP is %s.\n", deviceIP.toString().c_str());

  SPIFFS.begin();

  // Handle requests to the base path by showing a simple config page
  server->on("/", [&]() {
    File indexFile = SPIFFS.open("/index.html", "r");
    server->streamFile(indexFile, "text/html");

    if (server->hasArg("ssid") || server->hasArg("password")) {
      ssid = server->arg("ssid");
      password = server->arg("password");
      ssid.remove(settingsManager.maximumSettingLength - 1);
      password.remove(settingsManager.maximumSettingLength - 1);

      connectToWiFiNetwork(server->hasArg("notimeout"));
    }

    indexFile.close();
  });

  server->on("/status", [&]() {
    String connStatus;
    switch (WiFi.status()) {
      case WL_CONNECTED :
        if (ssid == "" || password == "") {
          connStatus = "Disconnected"; // We get WL_CONNECTED only with an AP connection, so this kind of deals with that
          break;
        }
        connStatus = "Connected";
        break;
      case WL_CONNECT_FAILED :
        connStatus = "Connection attempt failed";
        break;
      case WL_CONNECTION_LOST :
        connStatus = "Connection lost";
        break;
      case WL_DISCONNECTED :
        connStatus = "Disconnected";
        break;
      default :
        connStatus = "Other";
        break;
    }
    server->send(HTTP_CODE_OK, "text/html",
                 String("<html><body style='color: white; font-size: 14px; font-family: monospace;'>Network Name: ") + ssid +
                 "<br>Password: " + password +
                 "<br>Connection Status: " + connStatus +
                 "</body></html>");
  });

  server->on("/promptforexitsetup", [&]() {
    bool shouldPrompt = isConnectedToWiFi();
    server->send(shouldPrompt ? HTTP_CODE_OK : HTTP_CODE_INTERNAL_SERVER_ERROR, "text/plain", shouldPrompt ? "true" : "false");
  });

  server->on("/exitsetup", [&]() {
    if (!isConnectedToWiFi()) return;

    server->send(HTTP_CODE_OK, "text/plain", "Exiting setup...");

    setupMode = false;

    WiFi.softAPdisconnect(true);
    dnsServer.stop();
    server->stop();
    SPIFFS.end();

    delete server;

    settingsManager.setSetting(SettingsManager::Setting::SSID, ssid);
    settingsManager.setSetting(SettingsManager::Setting::PASSWORD, password);
    settingsManager.exitSetupMode();
  });

  server->on("/info", [&]() {
    server->send(HTTP_CODE_OK, "text/plain", VERSION + String("\n") + BUILD_INFO);
  });

  server->onNotFound([&]() {
    if (!handleRequestedFile(server->uri()))
      server->send(404, "text/plain", "404 Not Found");
  });

  // Handle requests to the /config path by changing configuration
  server->begin();

  Serial.println("HTTP server has been started.");
}

bool ConnectionManager::handleRequestedFile(String path) {
  if (path.endsWith("/")) path += "index.html";
  Serial.println(path);
  String contentType = getContentType(path);
  if (SPIFFS.exists(path)) {
    File file = SPIFFS.open(path, "r");
    server->streamFile(file, contentType);
    file.close();
    return true;
  }
  return false;
}

String ConnectionManager::getContentType(String filename){
  if(filename.endsWith(".htm")) return "text/html";
  else if(filename.endsWith(".html")) return "text/html";
  else if(filename.endsWith(".css")) return "text/css";
  else if(filename.endsWith(".js")) return "application/javascript";
  else if(filename.endsWith(".png")) return "image/png";
  else if(filename.endsWith(".gif")) return "image/gif";
  else if(filename.endsWith(".jpg")) return "image/jpeg";
  else if(filename.endsWith(".ico")) return "image/x-icon";
  return "text/plain";
}

bool ConnectionManager::ready() {
  return isConnectedToWiFi();
}

void ConnectionManager::update() {
  settingsManager.updateFullResetTimer();

  if (setupMode) {
    dnsServer.processNextRequest();
    server->handleClient();
  }
  // Periodically attempt to reconnect if we're not in setup mode, and still disconnected.
  else if (!isConnectedToWiFi() && lastPeriodicReconnectAttempt - millis() >= periodicReconnectDelay) connectToWiFiNetwork();
}

String ConnectionManager::get() {
  // If we're probably not connected, don't do anything (We have to check for ssid beacuse WL_CONNECTED isn't always reliable)
  if (!isConnectedToWiFi()) {
    Serial.println("GET aborted, ssid is blank and/or WiFi isn't connected.");
    return "";
  }

  int statusCode = http.GET();
  if (statusCode != HTTP_CODE_OK) {
    Serial.printf("Remote server returned a non-OK status code of %i.\n", statusCode);
    return "";
  }
  String payload = http.getString();
  
  connectionTimedOut = !http.begin(wifiClient, url);

  return payload; // This only returns the response body.
}

// noTimeout == true will result in an infinite connect loop if the network really
// doesn't exist. We just provide it so the user has an out if their network takes
// forever to connect. The user should reset the microcontroller if they wish to
// escape the loop.
void ConnectionManager::connectToWiFiNetwork(bool noTimeout /*= false, see header*/) {
  if (noTimeout)
    Serial.println("We will connect with no timeout. Reset the microcontroller to escape the infinte loop");

  lastPeriodicReconnectAttempt = millis();

  http.end();
  WiFi.disconnect();
  WiFi.begin(ssid.c_str(), password.c_str());
  connectionTimedOut = false;

  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - startTime > connectionTimeout && !noTimeout) {
      Serial.println("\nWiFi connection attempt timed out.");
      connectionTimedOut = true;
      return;
    }
    for (int i = 0; i < 500; i++) {
      delay(1);
      // This is a bit of a janky way to delay 500ms, but we really need to update the reset flag bits on time
      settingsManager.updateFullResetTimer();
    }
    Serial.print(".");
  }
  Serial.printf("\nConnected to WiFi with a private IP of %s.\n", WiFi.localIP().toString().c_str());

  // Make a connection to the remote server
  connectionTimedOut = !http.begin(wifiClient, url);
}

bool ConnectionManager::isConnectedToWiFi() {
  return ssid != "" && WiFi.status() == WL_CONNECTED && !connectionTimedOut;
}