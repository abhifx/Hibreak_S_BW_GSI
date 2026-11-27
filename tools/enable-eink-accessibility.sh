#!/system/bin/sh
# Enable E-ink Center's accessibility service without removing other services.
set -eu

PKG=com.hibreak.einkcenter
SVC="$PKG/$PKG.EinkAccessibilityService"

current=$(settings get secure enabled_accessibility_services 2>/dev/null || true)
[ "$current" = "null" ] && current=""

case ":$current:" in
    *":$SVC:"*) ;;
    *)
        if [ -n "$current" ]; then
            settings put secure enabled_accessibility_services "$current:$SVC"
        else
            settings put secure enabled_accessibility_services "$SVC"
        fi
        ;;
esac
settings put secure accessibility_enabled 1

# Sideloaded accessibility services can be blocked by restricted-settings policy.
# This appop is harmless when unsupported; report the resulting service list.
appops set "$PKG" ACCESS_RESTRICTED_SETTINGS allow >/dev/null 2>&1 || true

echo "enabled_accessibility_services=$(settings get secure enabled_accessibility_services)"
echo "accessibility_enabled=$(settings get secure accessibility_enabled)"
