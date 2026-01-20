import http.client
from http.server import BaseHTTPRequestHandler
import sys
# Import the WatchDog class we defined in the previous step
from watchdog import WatchDog

# --- 1. CONFIGURATION ---
GATEWAY_IP = "10.10.1.1"
GATEWAY_PORT = 8080

# --- 2. GENERATE DATABASE ---
# Creates a dict where user `i` follows all users from 0 to i-1
def generjte_followers(count):
    db = {}
    for i in range(count):
        # In C++: for (int j = 0; j < i; j++)
        db[i] = list(range(i))
    return db

FOLLOWERS_DB = generate_followers(100)

# --- 3. GATEWAY CLIENT HELPER ---
def call_gateway(path):
    """
    Equivalent to C++ CallGateway(path).
    Connects to the local Sidecar (Proxy) to fetch data.
    """
    try:
        # Connect to the Gateway listening on port 8080
        conn = http.client.HTTPConnection(GATEWAY_IP, GATEWAY_PORT, timeout=5)
        conn.request("GET", path)
        resp = conn.getresponse()
        if resp.status == 200:
            return resp.read().decode('utf-8')
        return "" # Handle error or empty response
    except Exception as e:
        print(f"[Follower] Gateway call failed: {e}", file=sys.stderr)
        return "Unknown"
    finally:
        conn.close()

# --- 4. HANDLER LOGIC ---
class FollowerHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        # Basic Routing: Check if path starts with /followers/
        prefix = "/followers/"
        if not self.path.startswith(prefix):
            self.send_error(404, "Not Found")
            return

        try:
            # Extract ID: /followers/123 -> 123
            user_id_str = self.path[len(prefix):]
            user_id = int(user_id_str)
            
            # Lookup in DB
            if user_id not in FOLLOWERS_DB:
                raise ValueError("User not found")
            
            followers = FOLLOWERS_DB[user_id]
            
            # Retrieve names from User Service via Gateway
            names = []
            for f_id in followers:
                # C++: CallGateway("/user/" + std::to_string(id))
                req_path = f"/user/{f_id}"
                name = call_gateway(req_path)
                names.append(name)
            
            # Join with ", "
            response_body = ", ".join(names)
            
            # Send Response
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(response_body)))
            self.end_headers()
            self.wfile.write(response_body.encode('utf-8'))
            
        except (ValueError, KeyError):
            # Handle non-integer ID or ID not found
            error_msg = "User does not exist"
            self.send_response(404)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(error_msg)))
            self.end_headers()
            self.wfile.write(error_msg.encode('utf-8'))

    # Mute standard logging to keep output clean
    def log_message(self, format, *args):
        return

if __name__ == "__main__":
    # "follower" -> creates /tmp/serverless/follower.sock
    w = WatchDog("follower", FollowerHandler)
    w.run()
