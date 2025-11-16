# Serverless

## Junction

### Function

This will warmup the serverless function.

```bash
cd ./build/junction
./junction_run ./samples/serverless/caladan_function.config --function_name sample --function_arg warmup_data -- ./samples/serverless/function
```

After the warmup completes, run in restored mode.

```bash
./junction_run ./samples/serverless/caladan_function.config --restore --function_name sample --function_arg restore -- .metadata .elf
```

### Client

Open a new terminal and send request to the serverless channel.

```bash
cd ./build/junction
./junction_run ./samples/serverless/caladan_client.config -- ./samples/serverless/client "your request"
```

You should see the server response if it was successful.
