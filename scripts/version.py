# Inyecta la version del firmware como macro FIRMWARE_VERSION, derivada de
# `git describe`. Se recalcula en cada compilacion, asi que no puede quedar
# desactualizada como quedaria una constante escrita a mano.
#
# Que produce:
#   v0.8                     build parada justo sobre el tag
#   v0.8-3-gabc1234          3 commits despues del tag (lo tipico en dev)
#   v0.8-3-gabc1234-dirty    ademas hay cambios sin commitear
#
# El sufijo "-dirty" es el dato que mas importa en una cueva ya desplegada:
# avisa que ese equipo tiene un binario que NO se puede reproducir desde el
# repo. Sin esto, un build de dev y un release tageado son indistinguibles
# mirando el equipo.
Import("env")

import subprocess


def version_de_git():
    try:
        salida = subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty"],
            stderr=subprocess.DEVNULL,
        )
        return salida.decode().strip()
    except Exception:
        # Sin git en el PATH, o compilando fuera de un repo (por ejemplo desde
        # un tarball). No es motivo para romper el build.
        return "desconocida"


env.Append(CPPDEFINES=[("FIRMWARE_VERSION", env.StringifyMacro(version_de_git()))])
