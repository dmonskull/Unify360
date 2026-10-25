"""quickstart: connect, see what the console supports, call a function, poke memory.

run:  python3 quickstart.py 192.168.1.100
or let it find the console on the lan with no argument.
"""
import sys

sys.path.insert(0, "..")
import x360


def main():
    if len(sys.argv) > 1:
        ip = sys.argv[1]
    else:
        print("searching the lan for a console...")
        found = x360.discovery.discover_nap()
        if not found:
            print("none found; pass an ip")
            return
        ip, name = found[0]
        print(f"using {name} at {ip}")

    xb = x360.Xbox(ip).connect()
    print("name       :", xb.xbdm.name())
    print("type       :", xb.xbdm.console_type())
    print("backends   :", xb.backends())      # which call methods are live
    print("title id   :", hex(xb.title_id()))

    # read the running title's modules
    for m in xb.xbdm.modules():
        print("  module   :", m.get("name"), hex(m.get("base", 0)))

    # call a function - the facade picks jrpc / xdrpc / xrpc for you
    # (example: a no-arg system query; swap in your own address or export)
    try:
        tid = xb.call(module="xam.xex", ordinal=463, ret="int")
        print("call xam.463:", hex(tid))
    except x360.XbdmError as e:
        print("call failed :", e)

    # a toast on screen
    xb.notify("unify360 connected")

    xb.close()


if __name__ == "__main__":
    main()
