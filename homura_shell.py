#!/usr/bin/env python3
import os
import sys
import signal
import readline
import shlex

# ================= GLOBAL STATE =================

HISTORY = []
HISTORY_FILE = os.path.expanduser("~/.homura_history")

ALIASES = {
    "install": "apt install",
    "remove": "apt remove",
    "update": "apt update",
    "upgrade": "apt upgrade",
    "search": "apt search",
    "install": "apt install"}


JOBS = []
LAST_STATUS = 0

# ================= UTILITIES =================

def command_exists(cmd):
    for path in os.environ.get("PATH", "").split(":"):
        full = os.path.join(path, cmd)
        if os.path.isfile(full) and os.access(full, os.X_OK):
            return True
    return False

def find_pkg_manager():
    for pm in ("apt", "apt-get", "pkg"):
        if command_exists(pm):
            return pm
    return None

# ================= SIGNAL HANDLING =================

def sigint_handler(sig, frame):
    print()

def sigtstp_handler(sig, frame):
    print()

signal.signal(signal.SIGINT, sigint_handler)
signal.signal(signal.SIGTSTP, sigtstp_handler)

# ================= PARSING =================

def parse_pipeline(tokens):
    pipeline = []
    current = []

    for t in tokens:
        if t == "|":
            pipeline.append(current)
            current = []
        else:
            current.append(t)

    pipeline.append(current)
    return pipeline

def handle_redirection(cmd):
    stdin = None
    stdout = None
    append = False

    if "<" in cmd:
        i = cmd.index("<")
        stdin = cmd[i + 1]
        del cmd[i:i+2]

    if ">>" in cmd:
        i = cmd.index(">>")
        stdout = cmd[i + 1]
        append = True
        del cmd[i:i+2]

    elif ">" in cmd:
        i = cmd.index(">")
        stdout = cmd[i + 1]
        del cmd[i:i+2]

    return cmd, stdin, stdout, append

# ================= BUILTINS =================

def builtin(cmd):
    global LAST_STATUS

    if not cmd:
        return True

    if cmd[0] == "cyboult":
        print('Hello buddy, I left an imprint on it.Just hit me up on Instagram: "cyboult."' )
        return True

    # ---- Directory ----
    if cmd[0] == "cd":
        os.chdir(cmd[1] if len(cmd) > 1 else os.environ["HOME"])
        return True

    if cmd[0] == "pwd":
        print(os.getcwd())
        return True

    if cmd[0] == "clear":
        os.system("clear")
        return True

    # ---- History ----
    if cmd[0] == "history":
        for i, c in enumerate(HISTORY, 1):
            print(f"{i}: {c}")
        return True
        
    if cmd[0] == "cyboult":
        print('Hello buddy, I left an imprint on it.Just hit me up on Instagram: "cyboult."' )
        return True

    # ---- Jobs ----
    if cmd[0] == "jobs":
        for i, j in enumerate(JOBS):
            print(f"[{i}] {j['cmd']}")
        return True

    if cmd[0] == "fg":
        idx = int(cmd[1])
        job = JOBS.pop(idx)
        os.kill(job["pid"], signal.SIGCONT)
        os.waitpid(job["pid"], 0)
        return True

    # ---- Env ----
    if cmd[0] == "export":
        for pair in cmd[1:]:
            k, v = pair.split("=", 1)
            os.environ[k] = v
        return True

    if cmd[0] == "unset":
        for v in cmd[1:]:
            os.environ.pop(v, None)
        return True

    # ---- Package Managers ----
    if cmd[0] in ("apt", "apt-get", "pkg"):
        pm = find_pkg_manager()
        if not pm:
            print("homura: no supported package manager found")
            return True
        os.execvp(pm, cmd)

    # ---- Exit ----
    if cmd[0] == "exit":
        readline.write_history_file(HISTORY_FILE)
        sys.exit(0)

    return False

# ================= EXECUTION =================

def execute_pipeline(pipeline, background):
    global LAST_STATUS
    prev_fd = None
    pids = []

    for i, cmd in enumerate(pipeline):
        cmd, stdin, stdout, append = handle_redirection(cmd)
        read_fd, write_fd = os.pipe() if i < len(pipeline) - 1 else (None, None)

        pid = os.fork()

        if pid == 0:  # CHILD
            if prev_fd:
                os.dup2(prev_fd, 0)
                os.close(prev_fd)

            if write_fd:
                os.dup2(write_fd, 1)
                os.close(write_fd)

            if stdin:
                fd = os.open(stdin, os.O_RDONLY)
                os.dup2(fd, 0)
                os.close(fd)

            if stdout:
                flags = os.O_WRONLY | os.O_CREAT
                flags |= os.O_APPEND if append else os.O_TRUNC
                fd = os.open(stdout, flags, 0o644)
                os.dup2(fd, 1)
                os.close(fd)

            try:
                os.execvp(cmd[0], cmd)
            except FileNotFoundError:
                print(f"homura: command not found: {cmd[0]}")
                os._exit(127)

        else:  # PARENT
            pids.append(pid)
            if prev_fd:
                os.close(prev_fd)
            if write_fd:
                os.close(write_fd)
            prev_fd = read_fd

    if background:
        JOBS.append({"pid": pids[-1], "cmd": " ".join(pipeline[0])})
    else:
        for pid in pids:
            _, status = os.waitpid(pid, 0)
            LAST_STATUS = os.WEXITSTATUS(status)

# ================= MAIN LOOP =================

def shell():
    if os.path.exists(HISTORY_FILE):
        readline.read_history_file(HISTORY_FILE)

    while True:
        try:
            cmdline = input("homura$ ").strip()
            if not cmdline:
                continue

            HISTORY.append(cmdline)

            tokens = shlex.split(cmdline)

            # Alias expansion
            if tokens[0] in ALIASES:
                tokens = shlex.split(ALIASES[tokens[0]]) + tokens[1:]

            background = tokens[-1] == "&"
            if background:
                tokens = tokens[:-1]

            pipeline = parse_pipeline(tokens)

            if builtin(pipeline[0]):
                continue

            execute_pipeline(pipeline, background)

        except KeyboardInterrupt:
            print()
        except EOFError:
            print()
            break
        except Exception as e:
            print("homura error:", e)

# ================= ENTRY =================

if __name__ == "__main__":
    shell()