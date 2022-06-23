use @printf[I32](fmt: Pointer[None] tag, ...)
use @pony_exitcode[None](code: I32) 

actor Main

  new create(env: Env) =>
    let printer = @{(fmt: Pointer[None] tag): I32 => @printf(fmt)}
    let chr_num = printer("Hello world!\n".cstring())
    @pony_exitcode(chr_num) // expected 13
