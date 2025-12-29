echo === Project .gdbinit loaded ===\n

define hook-run
  set follow-fork-mode child
end

set detach-on-fork on

catch exec
commands
set follow-fork-mode parent
continue
end
