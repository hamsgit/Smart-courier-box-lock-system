#include <WiFi.h>
#include <WebServer.h>

const char *ssid = "No Data";
const char *password = "12Prasanna1403";
const char *correctPassword = "1234";

WebServer server(80);
const int buttonPin = 27;
const int ledPin = 26;
bool isAuthenticated = false;

void handleRoot() {
  char msg[1000];
  snprintf(msg, 1000,
           "<html>\
  <head>\
    <meta name='viewport' content='width=device-width, initial-scale=1'>\
    <title>ESP32 Access Control </title>\
    <style>\
    html { font-family: Arial; text-align: center; }\
    h2 { font-size: 2.5rem; }\
    .status { font-size: 2.0rem; color: %s; }\
    </style>\
  </head>\
  <body>\
      <h2>ESP32 Access Control</h2>\
      <form action='/login' method='POST'>\
        <input type='password' name='pass' placeholder='Enter Password'>\
        <input type='submit' value='Submit'>\
      </form>\
      <p class='status'>%s</p>\
  </body>\
</html>",
           isAuthenticated ? "green" : "red",
           isAuthenticated ? "Access Granted! Door is opening..." : "Enter Password"
          );
  server.send(200, "text/html", msg);
}

void handleLogin() {
  if (server.hasArg("pass")) {
    if (server.arg("pass") == correctPassword) {
      isAuthenticated = true;
      blinkLED();
    } else {
      isAuthenticated = false;
    }
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void blinkLED() {
  for (int i = 0; i < 5; i++) {
    digitalWrite(ledPin, HIGH);
    delay(500);
    digitalWrite(ledPin, LOW);
    delay(500);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(buttonPin, INPUT_PULLUP);
  pinMode(ledPin, OUTPUT);

  WiFi.begin(ssid, password);
  Serial.println("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nConnected to WiFi");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  server.on("/", handleRoot);
  server.on("/login", HTTP_POST, handleLogin);
  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();
}
