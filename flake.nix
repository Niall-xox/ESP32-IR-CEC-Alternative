{
  description = "ESP32 IR Remote — PC daemon, firmware toolchain, and NixOS module";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system:
        f (import nixpkgs { inherit system; }));
    in
    {
      # -----------------------------------------------------------------------
      # Development shell
      #
      # Provides the toolchain only. The build itself is plain CMake and
      # PlatformIO, exactly as on any other distribution — this shell is a way
      # of acquiring the tools, not a replacement build system.
      #
      #   nix develop
      #   cmake -B daemon/build -S daemon && cmake --build daemon/build
      #   cd firmware && pio run
      # -----------------------------------------------------------------------
      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          packages = with pkgs; [
            cmake
            pkg-config
            gcc
            hidapi
            # sdbus-cpp (unsuffixed) is still 1.x in nixpkgs. LinuxPowerMonitor
            # requires the v2 API — see the daemon dependency notes in the brief.
            sdbus-cpp_2
            # The FHS-wrapped PlatformIO. The bare platformio-core will not work:
            # PlatformIO downloads a prebuilt xtensa-esp32s3-elf toolchain linked
            # against /lib64/ld-linux-x86-64.so.2, which does not exist on NixOS.
            platformio
            # Serial console for firmware debugging.
            picocom
          ];
        };
      });

      # -----------------------------------------------------------------------
      # Daemon package
      #
      # Calls the project's own CMakeLists rather than reimplementing the build
      # in Nix, so this cannot drift from what a Debian or Arch user gets.
      # -----------------------------------------------------------------------
      packages = forAllSystems (pkgs: rec {
        default = esp32-ir-daemon;

        esp32-ir-daemon = pkgs.stdenv.mkDerivation {
          pname = "esp32-ir-daemon";
          version = "0.3.0";

          src = ./daemon;

          nativeBuildInputs = with pkgs; [ cmake pkg-config ];
          buildInputs = with pkgs; [ hidapi sdbus-cpp_2 ];

          # The upstream CMakeLists has no install() rule — it is built and
          # copied by hand on FHS distributions. Install explicitly here rather
          # than patching the shared build file, which must stay portable.
          installPhase = ''
            runHook preInstall
            install -Dm755 esp32-ir-daemon $out/bin/esp32-ir-daemon
            install -Dm644 $src/99-esp32-ir-remote.rules \
              $out/lib/udev/rules.d/99-esp32-ir-remote.rules
            runHook postInstall
          '';

          meta = with pkgs.lib; {
            description = "Mirrors PC power state to a TV over IR, as a CEC alternative";
            homepage = "https://github.com/Niall-xox/ESP32-IR-CEC-Alternative";
            platforms = platforms.linux;
            mainProgram = "esp32-ir-daemon";
          };
        };
      });

      # -----------------------------------------------------------------------
      # NixOS module
      #
      # Replaces the manual install steps documented for FHS distributions:
      # the service account, udev rule and unit file are all declared here, so
      # they survive a nixos-rebuild (useradd/groupadd would not).
      #
      #   imports = [ inputs.esp32-ir-remote.nixosModules.default ];
      #   services.esp32-ir-remote.enable = true;
      # -----------------------------------------------------------------------
      nixosModules.default = { config, lib, pkgs, ... }:
        let cfg = config.services.esp32-ir-remote;
        in {
          options.services.esp32-ir-remote = {
            enable = lib.mkEnableOption "ESP32 IR Remote power-sync daemon";

            package = lib.mkOption {
              type = lib.types.package;
              default = self.packages.${pkgs.system}.esp32-ir-daemon;
              defaultText = lib.literalExpression "esp32-ir-daemon";
              description = "The daemon package to run.";
            };
          };

          config = lib.mkIf cfg.enable {
            users.groups.esp32ir = { };
            users.users.esp32ir = {
              isSystemUser = true;
              group = "esp32ir";
              description = "ESP32 IR Remote daemon";
            };

            # Ships 99-esp32-ir-remote.rules, granting the esp32ir group access
            # to the device's hidraw node.
            services.udev.packages = [ cfg.package ];

            systemd.services.esp32-ir-remote = {
              description = "ESP32 IR Remote Daemon";
              after = [ "systemd-logind.service" ];
              requires = [ "systemd-logind.service" ];
              wantedBy = [ "multi-user.target" ];

              serviceConfig = {
                Type = "simple";
                ExecStart = lib.getExe cfg.package;
                Restart = "on-failure";
                RestartSec = 5;

                User = "esp32ir";
                Group = "esp32ir";

                # Mirrors the hardening in esp32-ir-remote.service. If the daemon
                # ever cannot reach D-Bus, relax ProtectSystem first.
                NoNewPrivileges = true;
                ProtectSystem = "strict";
                ProtectHome = true;
                PrivateTmp = true;
                ProtectKernelTunables = true;
                ProtectKernelModules = true;
                ProtectControlGroups = true;
                RestrictNamespaces = true;
                LockPersonality = true;
                MemoryDenyWriteExecute = true;
                SystemCallArchitectures = "native";
                RestrictAddressFamilies = [ "AF_UNIX" ];
                DeviceAllow = [ "char-hidraw rw" ];
              };
            };
          };
        };
    };
}
