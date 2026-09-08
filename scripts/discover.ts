import { createSocket } from "node:dgram";
import { cli, main } from "./common.ts";
import { buildProbe, probeAddress } from "./probe.ts";

export async function discoverAddresses(): Promise<string[]> {
  const socket = createSocket("udp4");
  const ips = new Set<string>();
  try {
    await new Promise<void>((resolve, reject) => {
      let timer: NodeJS.Timeout | undefined;
      socket.on("error", () => {
        clearTimeout(timer);
        reject(
          new Error(
            "SSDP discovery failed. Check LAN permissions and network connectivity.",
          ),
        );
      });
      socket.on("message", (data, address) => {
        if (
          data.length <= 8192 &&
          data.toString().toLowerCase().includes("zoneplayer")
        )
          ips.add(address.address);
      });
      const request =
        'M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: "ssdp:discover"\r\nMX: 2\r\nST: urn:schemas-upnp-org:device:ZonePlayer:1\r\n\r\n';
      socket.send(request, 1900, "239.255.255.250", (error) => {
        if (error) reject(new Error("SSDP send failed"));
        else timer = setTimeout(resolve, 3000);
      });
    });
  } finally {
    socket.close();
  }
  return [...ips].sort();
}
export async function discover(
  addresses = discoverAddresses,
  probe?: (ip: string) => Promise<string>,
  print = console.log,
) {
  const ips = await addresses();
  if (!ips.length)
    throw new Error(
      "No SSDP replies. Check LAN permissions and multicast connectivity.",
    );
  if (!probe) {
    const binary = await buildProbe();
    probe = (ip) => probeAddress(binary, ip, undefined, true);
  }
  let succeeded = false;
  for (const ip of ips) {
    try {
      print(`SPEAKER ${ip}\n${await probe(ip)}`);
      succeeded = true;
    } catch {
      print(JSON.stringify({ ip, error: "Read-only probe failed" }));
    }
  }
  if (!succeeded) throw new Error("No discovered speakers could be read");
}
if (import.meta.main)
  await main(async () => {
    const { values, positionals } = cli([]);
    if (values.help) return console.log("node --run discover");
    if (positionals.length) throw new Error("Unexpected positional arguments");
    await discover();
  });
