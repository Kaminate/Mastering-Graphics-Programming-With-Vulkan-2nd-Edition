#!/usr/bin/env python3
"""Raptor Engine repository commands.

Run from anywhere: the repository root is the parent of this file's folder.
Windows: raptor.bat <command>. Linux: python3 tools/raptor.py <command>.
Only the Python standard library is used.

    raptor setup [--vs 2022|2026]      fresh clone: deps, check, generate
    raptor check                       check tools, SDK, submodule and sample models
    raptor deps                        init submodules and download the sample models
    raptor generate [--vs 2022|2026]   create the CMake project in project/
    raptor refresh                     re-run CMake on the existing project (new or removed files)
    raptor open                        open the generated solution
    raptor build [chapter] [--config]  build one chapter (14, Chapter14) or all of them
    raptor run <chapter> [--config]    run a chapter from bin/; arguments for the chapter after --
    raptor validate <chapter>          run a chapter with Vulkan validation, log and summary of the messages
    raptor shaders [--filter text]     compile every GLSL and Slang shader offline and report the errors
    raptor clean-shaders               delete the shader and pipeline cache in binaries/data/shaders
    raptor clean-temp                  delete the shader compiler temporary files
    raptor clean [--all]               delete project/ (and bin/ with --all)
    raptor check-cmake                 list sources missing from CMake, and CMake entries missing on disk

Without a command an interactive menu is shown.
"""

import argparse
import collections
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import tempfile

# Paths ///////////////////////////////////////////////////////////////////////

ROOT          = os.path.dirname( os.path.dirname( os.path.abspath( __file__ ) ) )
PROJECT_DIR   = os.path.join( ROOT, "project" )
BIN_DIR       = os.path.join( ROOT, "bin" )
SOURCE_DIR    = os.path.join( ROOT, "source" )
SHADER_CACHE  = os.path.join( ROOT, "binaries", "data", "shaders" )   # renderer.cpp: RAPTOR_DATA_FOLDER/shaders/
SHADERS_GLSL  = os.path.join( ROOT, "source", "shaders", "glsl" )
SHADERS_SLANG = os.path.join( ROOT, "source", "shaders", "slang" )
SPONZA        = os.path.join( ROOT, "deps", "src", "glTF-Sample-Models", "2.0", "Sponza", "glTF", "Sponza.gltf" )

IS_WINDOWS    = os.name == "nt"
CONFIGS       = [ "Debug", "Release", "RelWithDebInfo" ]

# Visual Studio major version -> CMake generator (README: VS 2022 and VS 2026 are supported).
VS_GENERATORS = { "2022": "Visual Studio 17 2022", "2026": "Visual Studio 18 2026" }
VS_MAJOR_TO_YEAR = { "17": "2022", "18": "2026" }

# Files written in the working directory by shader_compiler.cpp.
SHADER_TEMP_FILES = [ "temp.shader", "temp.slang", "shader_final.spv", "shader_opt.spv" ]
SHADER_TEMP_PATTERN = re.compile( r".*_(glsl|slang)\.spv$" )  # per-stage debug dumps, e.g. FRAGMENT_glsl.spv


# Helpers /////////////////////////////////////////////////////////////////////

def info( message ):
    print( message )

def fail( message ):
    print( "error: " + message )
    sys.exit( 1 )

def run_command( command, cwd = ROOT ):
    info( "> " + " ".join( '"{}"'.format( c ) if " " in c else c for c in command ) )
    return subprocess.call( command, cwd = cwd )

def run_or_fail( command, cwd = ROOT ):
    result = run_command( command, cwd )
    if result != 0:
        fail( "command failed with code {}".format( result ) )

def remove_tree( path ):
    if os.path.isdir( path ):
        shutil.rmtree( path )
        info( "removed " + os.path.relpath( path, ROOT ) )

def confirm( question, assume_yes ):
    if assume_yes:
        return True
    answer = input( question + " [y/N] " ).strip().lower()
    return answer in ( "y", "yes", "s", "si" )


# Visual Studio and CMake /////////////////////////////////////////////////////

def vswhere_path():
    program_files = os.environ.get( "ProgramFiles(x86)", r"C:\Program Files (x86)" )
    path = os.path.join( program_files, "Microsoft Visual Studio", "Installer", "vswhere.exe" )
    return path if os.path.isfile( path ) else None

def vswhere_query( property_name ):
    vswhere = vswhere_path()
    if vswhere is None:
        return None
    try:
        output = subprocess.check_output( [ vswhere, "-latest", "-products", "*",
                                            "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                                            "-property", property_name ], universal_newlines = True )
    except ( OSError, subprocess.CalledProcessError ):
        return None
    output = output.strip()
    return output if output else None

def find_cmake():
    cmake = shutil.which( "cmake" )
    if cmake:
        return cmake
    # Same fallback as generate_project.bat: the CMake bundled with Visual Studio.
    install_path = vswhere_query( "installationPath" ) if IS_WINDOWS else None
    if install_path:
        bundled = os.path.join( install_path, "Common7", "IDE", "CommonExtensions", "Microsoft", "CMake", "CMake", "bin", "cmake.exe" )
        if os.path.isfile( bundled ):
            return bundled
    return None

def cmake_or_fail():
    cmake = find_cmake()
    if cmake is None:
        fail( "CMake not found: install it or add it to PATH" )
    return cmake

def detect_vs_year():
    version = vswhere_query( "installationVersion" )
    if version:
        return VS_MAJOR_TO_YEAR.get( version.split( "." )[ 0 ] )
    return None

def cached_generator():
    cache = os.path.join( PROJECT_DIR, "CMakeCache.txt" )
    if not os.path.isfile( cache ):
        return None
    with open( cache, "r", errors = "replace" ) as f:
        for line in f:
            if line.startswith( "CMAKE_GENERATOR:INTERNAL=" ):
                return line.split( "=", 1 )[ 1 ].strip()
    return None

def is_multi_config( generator ):
    return generator is not None and ( generator.startswith( "Visual Studio" ) or generator == "Ninja Multi-Config" )


# Vulkan SDK //////////////////////////////////////////////////////////////////

def sdk_tool( name ):
    """A tool of the Vulkan SDK (glslangValidator, slangc), else the one in PATH."""
    executable = name + ( ".exe" if IS_WINDOWS else "" )
    sdk = os.environ.get( "VULKAN_SDK" )
    if sdk:
        path = os.path.join( sdk, "Bin" if IS_WINDOWS else "bin", executable )
        if os.path.isfile( path ):
            return path
    return shutil.which( name )


# Chapters ////////////////////////////////////////////////////////////////////

def chapter_name( value ):
    """'14', 'chapter14', 'Chapter14' -> 'Chapter14'."""
    digits = re.sub( r"(?i)^chapter", "", value.strip() )
    if not digits.isdigit():
        fail( "unknown chapter '{}': use a number like 14 or a name like Chapter14".format( value ) )
    return "Chapter{:02d}".format( int( digits ) )

def chapter_list():
    names = []
    for entry in sorted( os.listdir( SOURCE_DIR ) ):
        if re.match( r"^chapter\d+$", entry ) and os.path.isfile( os.path.join( SOURCE_DIR, entry, "main.cpp" ) ):
            names.append( chapter_name( entry ) )
    return names


# Commands ////////////////////////////////////////////////////////////////////

def command_check( args ):
    ok = True

    def report( name, good, detail ):
        nonlocal ok
        ok = ok and good
        info( "  [{}] {:<22} {}".format( "ok" if good else "!!", name, detail ) )

    info( "Repository: " + ROOT )
    report( "python", sys.version_info >= ( 3, 8 ), sys.version.split()[ 0 ] )

    cmake = find_cmake()
    if cmake:
        version = subprocess.check_output( [ cmake, "--version" ], universal_newlines = True ).splitlines()[ 0 ]
        report( "cmake", True, version + "  (" + cmake + ")" )
    else:
        report( "cmake", False, "not found" )

    git = shutil.which( "git" )
    report( "git", git is not None, git or "not found" )

    if IS_WINDOWS:
        year = detect_vs_year()
        report( "visual studio", year is not None, ( "VS " + year ) if year else "no VS 2022/2026 with C++ tools found" )

    sdk = os.environ.get( "VULKAN_SDK" )
    report( "VULKAN_SDK", sdk is not None and os.path.isdir( sdk ), sdk or "not set" )
    if sdk:
        lib_dir = os.path.join( sdk, "Lib" if IS_WINDOWS else "lib" )
        slang_name = "slang.lib" if IS_WINDOWS else "libslang.so"
        report( "slang in SDK", os.path.isfile( os.path.join( lib_dir, slang_name ) ), os.path.join( lib_dir, slang_name ) )
    glslang = sdk_tool( "glslangValidator" )
    report( "glslangValidator", glslang is not None, glslang or "not found" )
    slangc = sdk_tool( "slangc" )
    report( "slangc", slangc is not None, slangc or "not found (only 'raptor shaders' needs it)" )

    submodule = os.path.join( ROOT, "bootstrapping", "bootstrap.py" )
    report( "bootstrapping module", os.path.isfile( submodule ), "ok" if os.path.isfile( submodule ) else "missing: run 'raptor deps'" )
    report( "sample models", os.path.isfile( SPONZA ), "ok" if os.path.isfile( SPONZA ) else "missing: run 'raptor deps'" )

    generator = cached_generator()
    report( "project/", True, ( "generated with " + generator ) if generator else "not generated yet: run 'raptor generate'" )

    info( "" )
    info( "All good." if ok else "Some checks failed (!!)." )
    return 0 if ok else 1


def command_setup( args ):
    # deps first: before it the submodule and the sample models are missing and the check would fail on them.
    command_deps( args )
    info( "" )
    if command_check( args ) != 0:
        fail( "fix the checks marked !! and run 'raptor setup' again" )
    info( "" )
    args.fresh = False
    command_generate( args )
    info( "" )
    info( "Next: 'raptor build 14' and 'raptor run 14', or 'raptor open' for Visual Studio." )
    return 0


def command_deps( args ):
    git = shutil.which( "git" )
    if git is None:
        fail( "git not found" )
    run_or_fail( [ git, "submodule", "update", "--init", "--recursive" ] )
    run_or_fail( [ sys.executable, os.path.join( ROOT, "bootstrap.py" ) ] )
    info( "Sample models: " + ( "ok" if os.path.isfile( SPONZA ) else "still missing, check the bootstrap output" ) )
    return 0


def command_generate( args ):
    cmake = cmake_or_fail()

    if args.fresh:
        remove_tree( PROJECT_DIR )

    command = [ cmake, "-S", ROOT, "-B", PROJECT_DIR ]

    if IS_WINDOWS:
        year = args.vs or detect_vs_year() or "2022"
        generator = VS_GENERATORS[ year ]
        existing = cached_generator()
        if existing and existing != generator:
            fail( "project/ was generated with '{}'. Run 'raptor generate --fresh --vs {}' to switch.".format( existing, year ) )
        command += [ "-G", generator, "-A", "x64" ]
    elif args.ninja or ( shutil.which( "ninja" ) and cached_generator() is None ):
        command += [ "-G", "Ninja Multi-Config" ]

    run_or_fail( command )
    info( "Project ready in " + os.path.relpath( PROJECT_DIR, ROOT ) )
    return 0


def command_refresh( args ):
    if cached_generator() is None:
        info( "No project yet: generating it." )
        args.fresh = False
        args.vs = None
        args.ninja = False
        return command_generate( args )
    # Re-running configure keeps the generator stored in CMakeCache.txt.
    run_or_fail( [ cmake_or_fail(), "-S", ROOT, "-B", PROJECT_DIR ] )
    return 0


def command_open( args ):
    if cached_generator() is None:
        fail( "no project yet: run 'raptor generate'" )
    run_or_fail( [ cmake_or_fail(), "--open", PROJECT_DIR ] )
    return 0


def command_build( args ):
    generator = cached_generator()
    if generator is None:
        fail( "no project yet: run 'raptor generate'" )
    command = [ cmake_or_fail(), "--build", PROJECT_DIR, "--parallel" ]
    if is_multi_config( generator ):
        command += [ "--config", args.config ]
    if args.chapter:
        command += [ "--target", chapter_name( args.chapter ) ]
    run_or_fail( command )
    return 0


def chapter_command( args ):
    name = chapter_name( args.chapter )
    executable = os.path.join( BIN_DIR, "{}_{}{}".format( name, args.config, ".exe" if IS_WINDOWS else "" ) )
    if not os.path.isfile( executable ):
        fail( "{} not found: run 'raptor build {} --config {}'".format( os.path.relpath( executable, ROOT ), args.chapter, args.config ) )
    return [ executable ] + args.extra


def command_run( args ):
    # bin/ is the working directory the code expects: the default model is ../deps/..., the shader
    # compiler passes -I../source/shaders/glsl and writes its temporary files in the working directory.
    return run_command( chapter_command( args ), cwd = BIN_DIR )


# gpu_device.cpp debug_utils_callback prints " MessageID: <name> <number>" for every message.
MESSAGE_ID_PATTERN = re.compile( r"MessageID:\s*(\S+)" )

def command_validate( args ):
    command = chapter_command( args )
    log_path = os.path.join( BIN_DIR, "validation_{}_{}.log".format( chapter_name( args.chapter ), args.config ) )

    # Read and removed from argv by AppSettings::load: it replaces /gpu/debug of the settings files for this run.
    command = [ command[ 0 ], "--gpu-debug", args.mode ] + command[ 1: ]
    info( "> " + " ".join( command ) )

    message_ids = collections.Counter()
    with open( log_path, "w", encoding = "utf-8", errors = "replace" ) as log:
        process = subprocess.Popen( command, cwd = BIN_DIR, stdout = subprocess.PIPE,
                                    stderr = subprocess.STDOUT, universal_newlines = True, errors = "replace" )
        for line in process.stdout:
            sys.stdout.write( line )
            log.write( line )
            match = MESSAGE_ID_PATTERN.search( line )
            if match:
                message_ids[ match.group( 1 ) ] += 1
        result = process.wait()

    info( "" )
    info( "Exit code {}. Log: {}".format( result, os.path.relpath( log_path, ROOT ) ) )
    if not message_ids:
        info( "No validation messages." )
        return result
    info( "{} messages, {} different (count, id):".format( sum( message_ids.values() ), len( message_ids ) ) )
    for message_id, count in message_ids.most_common():
        info( "  {:>6}  {}".format( count, message_id ) )
    return result if result != 0 else 1


# Shaders /////////////////////////////////////////////////////////////////////
#
# The engine compiles one entry point at a time: the source file plus the defines <STAGE>_<NAME>, <STAGE> and
# RAPTOR_GLSL or RAPTOR_SLANG (shader_compiler.cpp). The entry points of a file are its #if defined(<STAGE>_<NAME>)
# blocks; a file without them uses bare #ifdef <STAGE> blocks, or its extension (.vert, .frag).
# A GLSL file without #version gets headers prepended by the engine: the lists come from the .headers of the C++
# stage creations, with the same paths (relative to source/shaders). Such a file not created by the C++ code is skipped.
# Defines added by the C++ code (variants) are not known here: only the default variant is compiled.

STAGES = [ "VERTEX", "FRAGMENT", "COMPUTE", "MESH", "TASK", "RAYGEN", "CLOSEST_HIT", "ANY_HIT", "MISS", "INTERSECTION" ]
STAGE_PATTERN = re.compile( r"(?:defined\s*\(?|#\s*ifn?def)\s*((?:" + "|".join( STAGES ) + r")(?:_[A-Z0-9_]+)?)\b" )
EXTENSION_STAGES = { ".vert": "VERTEX", ".frag": "FRAGMENT", ".comp": "COMPUTE", ".mesh": "MESH", ".task": "TASK" }

GLSL_STAGE_NAMES = { "VERTEX": "vert", "FRAGMENT": "frag", "COMPUTE": "comp", "MESH": "mesh", "TASK": "task", "RAYGEN": "rgen",
                     "CLOSEST_HIT": "rchit", "ANY_HIT": "rahit", "MISS": "rmiss", "INTERSECTION": "rint" }
# Same profiles as to_slang_shader_profiler() in shader_compiler.cpp.
# slangc stage names. The engine asks profiles like cs_6_5 (to_slang_shader_profiler): sm_6_5 plus the stage.
SLANG_STAGES = { "VERTEX": "vertex", "FRAGMENT": "fragment", "COMPUTE": "compute", "MESH": "mesh", "TASK": "amplification",
                 "RAYGEN": "raygeneration", "CLOSEST_HIT": "closesthit", "ANY_HIT": "anyhit", "MISS": "miss",
                 "INTERSECTION": "intersection" }

def stage_of( define ):
    for stage in sorted( STAGES, key = len, reverse = True ):
        if define == stage or define.startswith( stage + "_" ):
            return stage
    return None

DIRECTIVE_PATTERN = re.compile( r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)" )

def shader_entry_points( path ):
    """(define or None, stage) for each entry point of a shader file. Stage defines nested inside a block that
    already tests a stage define (#if defined(TASK_DEPTH_CUBEMAP) inside TASK_..._SLANG) are options, not entries."""
    found = []
    stack = []   # one bool per open #if: does it test a stage define
    with open( path, "r", errors = "replace" ) as f:
        for line in f:
            match = DIRECTIVE_PATTERN.match( line )
            if not match:
                continue
            directive = match.group( 1 )
            if directive == "endif":
                if stack:
                    stack.pop()
                continue
            defines = STAGE_PATTERN.findall( line )
            if directive in ( "if", "ifdef", "ifndef" ):
                if defines and not any( stack ):
                    found += defines
                stack.append( bool( defines ) )
            elif stack:   # elif, else
                if defines and not any( stack[ :-1 ] ):
                    found += defines
                stack[ -1 ] = stack[ -1 ] or bool( defines )
    found = sorted( set( found ) )
    named = [ d for d in found if d not in STAGES ]
    if named:
        return [ ( d, stage_of( d ) ) for d in named ]
    if found:
        return [ ( None, d ) for d in found ]
    stage = EXTENSION_STAGES.get( os.path.splitext( path )[ 1 ] )
    return [ ( None, stage ) ] if stage else []

SHADERS_DIR = os.path.join( SOURCE_DIR, "shaders" )   # RAPTOR_SHADER_FOLDER: stage sources and headers are relative to it

def shader_key( path ):
    return os.path.relpath( path, SHADERS_DIR ).replace( "\\", "/" )

def glsl_header_lists():
    """Shader path relative to source/shaders -> headers the engine prepends, from the C++ stage creations."""
    headers = collections.defaultdict( list )
    creation = re.compile( r'\.glsl\s*=\s*"([^"]+)"\s*\}\s*,\s*\.headers\s*=\s*\{([^}]*)\}' )
    for dirpath, _, filenames in os.walk( SOURCE_DIR ):
        for name in filenames:
            if not name.endswith( ( ".cpp", ".hpp" ) ):
                continue
            with open( os.path.join( dirpath, name ), "r", errors = "replace" ) as f:
                text = re.sub( r"/\*.*?\*/", "", f.read(), flags = re.S )
            for shader, header_list in creation.findall( text ):
                for header in re.findall( r'"([^"]+)"', header_list ):
                    if header not in headers[ shader ]:
                        headers[ shader ].append( header )
    return headers

def cpp_string_literals():
    """Every "string" of the C++ code, lowercase: the pipeline names the stage defines are made from."""
    literals = set()
    for dirpath, _, filenames in os.walk( SOURCE_DIR ):
        for name in filenames:
            if name.endswith( ( ".cpp", ".hpp" ) ):
                with open( os.path.join( dirpath, name ), "r", errors = "replace" ) as f:
                    for literal in re.findall( r'"([A-Za-z0-9_./]+)"', f.read() ):
                        literals.add( literal.lower() )
                        literals.add( os.path.basename( literal ).lower() )
    return literals

def has_version( path ):
    with open( path, "r", errors = "replace" ) as f:
        return re.search( r"^\s*#\s*version\b", f.read(), re.M ) is not None

def concatenate_headers( path, headers, output_path ):
    """Same as the engine: the headers, then the shader. #line 1 keeps the error lines of the shader file.
    Returns the headers that do not exist (the engine prints "Cannot read file" and goes on without them)."""
    missing = []
    with open( output_path, "w" ) as out:
        for header in headers:
            header_path = os.path.join( SHADERS_DIR, header )
            if not os.path.isfile( header_path ):
                missing.append( header )
                continue
            with open( header_path, "r", errors = "replace" ) as f:
                out.write( f.read() + "\n" )
        with open( path, "r", errors = "replace" ) as f:
            out.write( "#line 1\n" + f.read() )
    return missing

def shader_files( folder, extensions ):
    result = []
    for dirpath, dirnames, filenames in os.walk( folder ):
        dirnames.sort()
        for name in sorted( filenames ):
            if name.endswith( extensions ):
                result.append( os.path.join( dirpath, name ) )
    return result

def glsl_command( compiler, path, define, stage, output ):
    command = [ compiler, path, "-V", "--target-env", "vulkan1.3", "-S", GLSL_STAGE_NAMES[ stage ], "-o", output,
                "-I" + SHADERS_GLSL, "-D" + stage, "-DRAPTOR_GLSL" ]
    return command + ( [ "-D" + define ] if define else [] )

def slang_command( compiler, path, define, stage, output ):
    command = [ compiler, path, "-target", "spirv", "-profile", "sm_6_5", "-entry", "main", "-stage", SLANG_STAGES[ stage ],
                "-emit-spirv-directly", "-matrix-layout-column-major", "-o", output,
                "-I", SHADERS_SLANG, "-I", os.path.join( SHADERS_SLANG, "chapter15" ), "-D" + stage, "-DRAPTOR_SLANG" ]
    return command + ( [ "-D" + define ] if define else [] )

def command_shaders( args ):
    jobs = []   # ( label, compiler, command builder, path, define, stage, headers )
    skipped = []
    header_lists = glsl_header_lists()
    literals = cpp_string_literals()
    languages = [ ( "glsl", SHADERS_GLSL, ( ".glsl", ".vert", ".frag", ".comp" ), "glslangValidator", glsl_command ),
                  ( "slang", SHADERS_SLANG, ( ".slang", ), "slangc", slang_command ) ]
    for language, folder, extensions, tool, make_command in languages:
        if args.language and args.language != language:
            continue
        compiler = sdk_tool( tool )
        if compiler is None:
            info( "{} not found: {} shaders skipped (check VULKAN_SDK)".format( tool, language ) )
            continue
        for path in shader_files( folder, extensions ):
            relative = os.path.relpath( path, ROOT )
            entry_points = shader_entry_points( path )
            headers = []
            if language == "glsl" and not has_version( path ):
                headers = header_lists.get( shader_key( path ), [] )
                if not headers:
                    skipped.append( relative + " (no #version and no C++ stage with headers uses this path)" )
                    continue
            if not entry_points:
                skipped.append( relative + " (no entry point)" )
            file_used = os.path.basename( path ).lower() in literals
            for define, stage in entry_points:
                label = "{} [{}]".format( relative, define or stage )
                if not file_used:
                    label += "  (file not named in the C++ code: probably unused)"
                if args.filter and args.filter.lower() not in label.lower():
                    continue
                elif define and define[ len( stage ) + 1: ].lower() not in literals:
                    label += "  (no C++ name '{}': probably unused)".format( define[ len( stage ) + 1: ].lower() )
                jobs.append( ( label, compiler, make_command, path, define, stage, headers ) )

    if not jobs:
        info( "No shaders to compile." )
        return 0

    info( "Compiling {} entry points...".format( len( jobs ) ) )
    with tempfile.TemporaryDirectory( prefix = "raptor_shaders_" ) as output_dir:
        def compile_one( index_job ):
            index, ( label, compiler, make_command, path, define, stage, headers ) = index_job
            original = path
            output = os.path.join( output_dir, "{}.spv".format( index ) )
            notes = ""
            if headers:
                source = os.path.join( output_dir, "{}_{}".format( index, os.path.basename( path ) ) )
                missing = concatenate_headers( path, headers, source )
                if missing:
                    notes = "header not found in source/shaders: {}\n".format( ", ".join( missing ) )
                path = source
            # The temporary folder is also the working directory: nothing is written in the repository.
            command = make_command( compiler, path, define, stage, output ) + [ "-I" + os.path.dirname( original ) ]
            completed = subprocess.run( command, cwd = output_dir,
                                        stdout = subprocess.PIPE, stderr = subprocess.STDOUT, universal_newlines = True,
                                        errors = "replace" )
            ok = completed.returncode == 0 and os.path.isfile( output )
            return label, ok, notes + completed.stdout

        workers = min( 32, ( os.cpu_count() or 4 ) )
        with concurrent.futures.ThreadPoolExecutor( max_workers = workers ) as pool:
            results = list( pool.map( compile_one, enumerate( jobs ) ) )

    failed = [ ( label, output ) for label, ok, output in results if not ok ]
    for label, output in failed:
        info( "FAILED " + label )
        for line in output.strip().splitlines():
            if line.strip() and not line.startswith( output_dir ):
                info( "    " + line )
    info( "" )
    if skipped and not args.filter:
        info( "Skipped:" )
        for line in skipped:
            info( "  " + line )
    unused = sum( 1 for label, _ in failed if "probably unused" in label )
    info( "{} compiled, {} failed{}.".format( len( results ) - len( failed ), len( failed ),
                                             " ({} probably unused)".format( unused ) if unused else "" ) )
    return 1 if failed else 0


def command_clean_shaders( args ):
    if not os.path.isdir( SHADER_CACHE ):
        info( "No shader cache in " + os.path.relpath( SHADER_CACHE, ROOT ) )
        return 0
    count = 0
    for entry in os.listdir( SHADER_CACHE ):
        path = os.path.join( SHADER_CACHE, entry )
        if os.path.isfile( path ):
            os.remove( path )
            count += 1
        elif os.path.isdir( path ):
            shutil.rmtree( path )
            count += 1
    info( "Removed {} entries from {}".format( count, os.path.relpath( SHADER_CACHE, ROOT ) ) )
    return 0


def command_clean_temp( args ):
    # The compiler writes in the current working directory: bin/ when run with 'raptor run',
    # project/source/chapterXX when run from Visual Studio, the root when run from there.
    folders = [ ROOT, BIN_DIR ]
    project_sources = os.path.join( PROJECT_DIR, "source" )
    if os.path.isdir( project_sources ):
        folders += [ os.path.join( project_sources, d ) for d in os.listdir( project_sources ) ]
    count = 0
    for folder in folders:
        if not os.path.isdir( folder ):
            continue
        for entry in os.listdir( folder ):
            if entry in SHADER_TEMP_FILES or SHADER_TEMP_PATTERN.match( entry ):
                path = os.path.join( folder, entry )
                if os.path.isfile( path ):
                    os.remove( path )
                    info( "removed " + os.path.relpath( path, ROOT ) )
                    count += 1
    info( "Removed {} temporary files.".format( count ) )
    return 0


def command_clean( args ):
    targets = [ PROJECT_DIR ] + ( [ BIN_DIR ] if args.all else [] )
    existing = [ t for t in targets if os.path.isdir( t ) ]
    if not existing:
        info( "Nothing to clean." )
        return 0
    names = ", ".join( os.path.relpath( t, ROOT ) + "/" for t in existing )
    if not confirm( "Delete " + names + "?", args.yes ):
        info( "Cancelled." )
        return 0
    for target in existing:
        remove_tree( target )
    return 0


SOURCE_EXTENSIONS = ( ".cpp", ".c", ".cc" )
HEADER_EXTENSIONS = ( ".hpp", ".h", ".inl" )
CMAKE_PATH_PATTERN = re.compile( r"[\w./+-]+\.(?:cpp|cc|c|hpp|h|inl)\b" )

def cmake_files_in_build():
    """Root CMakeLists.txt plus the add_subdirectory() folders it lists, as (folder, text)."""
    result = []
    pending = [ ROOT ]
    while pending:
        folder = pending.pop()
        path = os.path.join( folder, "CMakeLists.txt" )
        if not os.path.isfile( path ):
            continue
        with open( path, "r", errors = "replace" ) as f:
            text = f.read()
        result.append( ( folder, text ) )
        for sub in re.findall( r"add_subdirectory\s*\(\s*([^\s)]+)", text ):
            pending.append( os.path.normpath( os.path.join( folder, sub ) ) )
    return result

def checked_folders():
    """Engine and chapter code. Third party code, shaders and source/extra are not checked:
    they are compiled through other files (TracyClient.cpp), by the shader compiler, or not at all."""
    folders = [ os.path.join( SOURCE_DIR, "raptor" ) ]
    folders += [ os.path.join( SOURCE_DIR, d ) for d in sorted( os.listdir( SOURCE_DIR ) ) if re.match( r"^chapter\d+$", d ) ]
    return folders

def command_check_cmake( args ):
    referenced = set()
    missing_on_disk = []
    for folder, text in cmake_files_in_build():
        for match in CMAKE_PATH_PATTERN.findall( text ):
            path = os.path.normpath( os.path.join( folder, match ) )
            referenced.add( os.path.normcase( path ) )
            if not os.path.isfile( path ):
                missing_on_disk.append( os.path.relpath( path, ROOT ) )

    sources_not_built = []
    headers_not_listed = []
    for top in checked_folders():
        for dirpath, dirnames, filenames in os.walk( top ):
            dirnames[:] = [ d for d in dirnames if d != "generated" ]  # chapter15 headers generated from Slang
            for name in filenames:
                path = os.path.join( dirpath, name )
                if os.path.normcase( path ) in referenced:
                    continue
                if name.endswith( SOURCE_EXTENSIONS ):
                    sources_not_built.append( os.path.relpath( path, ROOT ) )
                elif name.endswith( HEADER_EXTENSIONS ):
                    headers_not_listed.append( os.path.relpath( path, ROOT ) )

    def print_list( title, paths ):
        info( "{} ({}):".format( title, len( paths ) ) )
        for path in sorted( set( paths ) ):
            info( "  " + path )
        info( "" )

    print_list( "Sources not compiled by any target", sources_not_built )
    print_list( "Headers not listed (they compile, but are missing from the IDE project)", headers_not_listed )
    print_list( "In CMakeLists.txt but missing on disk", missing_on_disk )
    return 1 if missing_on_disk else 0


# Interactive menu ////////////////////////////////////////////////////////////

MENU = [
    ( "setup",         "fresh clone: deps, check, generate",      [] ),
    ( "check",         "check tools, SDK and assets",             [] ),
    ( "deps",          "download submodules and sample models",   [] ),
    ( "generate",      "create the CMake project",                [] ),
    ( "refresh",       "re-run CMake on the project",             [] ),
    ( "open",          "open the solution",                       [] ),
    ( "build",         "build a chapter (asks which)",            [ "chapter" ] ),
    ( "run",           "run a chapter (asks which)",              [ "chapter" ] ),
    ( "validate",      "run a chapter with Vulkan validation",    [ "chapter" ] ),
    ( "shaders",       "compile all shaders offline",             [] ),
    ( "clean-shaders", "delete the shader cache",                 [] ),
    ( "clean-temp",    "delete shader compiler temporary files",  [] ),
    ( "check-cmake",   "list files missing from CMake",           [] ),
]

def interactive_menu():
    info( "Raptor commands:" )
    for index, ( name, description, _ ) in enumerate( MENU, 1 ):
        info( "  {:>2}. {:<14} {}".format( index, name, description ) )
    choice = input( "Number (empty to exit): " ).strip()
    if not choice.isdigit() or not ( 1 <= int( choice ) <= len( MENU ) ):
        return None
    name, _, asks = MENU[ int( choice ) - 1 ]
    argv = [ name ]
    if "chapter" in asks:
        chapters = chapter_list()
        answer = input( "Chapter ({}-{}, empty for all when building): ".format( chapters[ 0 ][ -2: ], chapters[ -1 ][ -2: ] ) ).strip()
        if answer:
            argv.append( answer )
        elif name != "build":
            return None
        default_config = "Debug" if name == "validate" else "Release"
        config = input( "Config [{}]: ".format( default_config ) ).strip() or default_config
        argv += [ "--config", config ]
    return argv


# Entry point /////////////////////////////////////////////////////////////////

def build_parser():
    parser = argparse.ArgumentParser( prog = "raptor", description = "Raptor Engine repository commands." )
    sub = parser.add_subparsers( dest = "command" )

    p = sub.add_parser( "setup", help = "fresh clone: deps, check, generate" )
    p.add_argument( "--vs", choices = sorted( VS_GENERATORS ), help = "Visual Studio version (default: the installed one)" )
    p.add_argument( "--ninja", action = "store_true", help = "Linux: use Ninja Multi-Config" )
    p.set_defaults( func = command_setup )

    sub.add_parser( "check", help = "check tools, SDK, submodule and sample models" ).set_defaults( func = command_check )
    sub.add_parser( "deps", help = "init submodules and download the sample models" ).set_defaults( func = command_deps )

    p = sub.add_parser( "generate", help = "create the CMake project in project/" )
    p.add_argument( "--vs", choices = sorted( VS_GENERATORS ), help = "Visual Studio version (default: the installed one)" )
    p.add_argument( "--fresh", action = "store_true", help = "delete project/ first" )
    p.add_argument( "--ninja", action = "store_true", help = "Linux: use Ninja Multi-Config" )
    p.set_defaults( func = command_generate )

    sub.add_parser( "refresh", help = "re-run CMake on the existing project" ).set_defaults( func = command_refresh )
    sub.add_parser( "open", help = "open the generated solution" ).set_defaults( func = command_open )

    p = sub.add_parser( "build", help = "build one chapter or all of them" )
    p.add_argument( "chapter", nargs = "?", help = "14 or Chapter14; empty builds everything" )
    p.add_argument( "--config", choices = CONFIGS, default = "Release" )
    p.set_defaults( func = command_build )

    p = sub.add_parser( "run", help = "run a chapter from bin/" )
    p.add_argument( "chapter", help = "14 or Chapter14" )
    p.add_argument( "--config", choices = CONFIGS, default = "Release" )
    p.set_defaults( func = command_run )

    p = sub.add_parser( "validate", help = "run a chapter with Vulkan validation and summarize the messages" )
    p.add_argument( "chapter", help = "14 or Chapter14" )
    p.add_argument( "--config", choices = CONFIGS, default = "Debug" )
    p.add_argument( "--mode", choices = [ "Validation", "GpuAssisted" ], default = "Validation",
                    help = "Validation: core + sync + best practices; GpuAssisted: GPU-assisted validation" )
    p.set_defaults( func = command_validate )

    p = sub.add_parser( "shaders", help = "compile every shader entry point offline" )
    p.add_argument( "--filter", help = "only entry points whose file or define contains this text" )
    p.add_argument( "--language", choices = [ "glsl", "slang" ], help = "only one language" )
    p.set_defaults( func = command_shaders )

    sub.add_parser( "clean-shaders", help = "delete binaries/data/shaders" ).set_defaults( func = command_clean_shaders )
    sub.add_parser( "clean-temp", help = "delete the shader compiler temporary files" ).set_defaults( func = command_clean_temp )

    p = sub.add_parser( "clean", help = "delete project/ (and bin/ with --all)" )
    p.add_argument( "--all", action = "store_true", help = "also delete bin/" )
    p.add_argument( "--yes", "-y", action = "store_true", help = "do not ask for confirmation" )
    p.set_defaults( func = command_clean )

    sub.add_parser( "check-cmake", help = "list sources missing from CMake" ).set_defaults( func = command_check_cmake )
    return parser


def main():
    argv = sys.argv[ 1: ]
    if not argv:
        argv = interactive_menu()
        if argv is None:
            return 0
    # Everything after "--" goes to the chapter (run, validate). Split by hand: an argparse REMAINDER after the
    # chapter would also swallow the options written after it (raptor run 14 --config Debug).
    extra = []
    if "--" in argv:
        extra = argv[ argv.index( "--" ) + 1: ]
        argv = argv[ :argv.index( "--" ) ]
    parser = build_parser()
    args = parser.parse_args( argv )
    args.extra = extra
    if args.command is None:
        parser.print_help()
        return 0
    return args.func( args )


if __name__ == "__main__":
    sys.exit( main() )
