assert easy_cxx_is_root

@brutal.rule(caching='process', traced=1)
def c_compiler():
  while 1:
    cc = brutal.env('CC', [])
    if cc: break

    NERSC_HOST = brutal.env('NERSC_HOST', None)
    if NERSC_HOST:
      cc = ['cc']
      break

    cc = ['gcc']
    break

  ver_text = brutal.process(cc + ['--version'], show=0, capture_stdout=1).wait()
  brutal.depend_fact('CC --version', ver_text)
  return cc

@brutal.rule(caching='process', traced=1)
def cxx_compiler():
  while 1:
    cxx = brutal.env('CXX', [])
    if cxx: break

    NERSC_HOST = brutal.env('NERSC_HOST', None)
    if NERSC_HOST:
      cxx = ['CC']
      break

    cxx = ['g++']
    break

  text = brutal.process(cxx + ['--version'], show=0, capture_stdout=1)
  text = text.wait()
  brutal.depend_fact('CXX --version', text)

  text = brutal.process(
    cxx + ['-x','c++','-E','-'],
    stdin='__cplusplus', capture_stdout=1, show=0
  )
  text = text.wait()

  for line in text.split('\n'):
    if line and not line.startswith('#'):
      std_ver = int(line.rstrip('L'))
      if std_ver < 201400:
        return cxx + ['-std=c++14']
      else:
        return cxx

  brutal.error('Invalid preprocessor output:', text)

@brutal.rule
def sources_from_includes_enabled(PATH):
  return brutal.os.path_within_any(PATH, brutal.here('src'), brutal.here('test'))

def code_context_base():
  debug = brutal.env('debug', 0)
  optlev = brutal.env('optlev', 0 if debug else 3)
  syms = brutal.env('syms', 1 if debug else 0)
  
  return CodeContext(
    compiler = cxx_compiler(),
    pp_angle_dirs = [brutal.here('src')],
    cg_optlev = optlev,
    cg_misc = ['-Wno-aligned-new','-march=native'] + (['-g'] if syms else []),
    pp_defines = {
      'DEBUG': 1 if debug else 0,
      'NDEBUG': None if debug else 1
    }
  )

@brutal.rule(traced=1)
def code_context(PATH):
  cxt = code_context_base()
  
  if PATH == brutal.here('src/devastator/world.hxx'):
    world = brutal.env('world', default='threads')

    if world == 'threads':
      cxt |= CodeContext(pp_defines={
        'WORLD_THREADS': 1,
        'RANK_N': brutal.env('ranks',2)
      })
    elif world == 'gasnet':
      cxt |= CodeContext(pp_defines={
        'WORLD_GASNET': 1,
        'PROCESS_N': brutal.env('procs',2),
        'WORKER_N': brutal.env('workers',2)
      })
    
  elif PATH == brutal.here('src/devastator/diagnostic.cxx'):
    version = brutal.process(
        ['git','describe','--dirty','--always','--tags'],
        capture_stdout=1, show=0, cwd=brutal.here()
      ).wait().strip()

    cxt |= CodeContext(pp_defines={'DEVA_GIT_VERSION': '"'+version+'"'})

  return brutal.complete_and_partial(cxt, cxt.with_pp_defines(DEVA_GIT_VERSION=None))
