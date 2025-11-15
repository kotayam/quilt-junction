# Serverless

## Junction

### Function

This will start the serverless function.

```bash
cd ./build/junction
./junction_run ./samples/serverless/caladan_function.config --function_arg warmup_data --keep_alive -- ./samples/serverless/function
```

### Client

Open a new terminal and send request to the serverless channel.

```bash
cd ./build/junction
./client "Your Message"
```

You should see the server response if it was successful.
