import sys
try:
    with open("check_env.log", "w") as f:
        f.write("Environment check successful!\n")
        f.write(sys.version + "\n")
    print("Environment check complete.")
except Exception as e:
    print(f"Error: {e}")
