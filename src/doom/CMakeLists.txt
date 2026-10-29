add_library(doom STATIC
            am_map.c        am_map.h
            deh_ammo.c
            deh_bexstr.c
            deh_cheat.c
            deh_doom.c
            deh_frame.c
            deh_misc.c      deh_misc.h
            deh_ptr.c
            deh_sound.c
            deh_thing.c
            deh_weapon.c
                            d_englsh.h
            ../diablo/d_diablo.c      ../diablo/d_diablo.h
            ../diablo/d_diablo_ui.c   ../diablo/d_diablo_ui.h
            ../diablo/d_diablo_icons.c ../diablo/d_diablo_icons.h
            ../diablo/d_pistol_sprite.c ../diablo/d_pistol_sprite.h
            ../diablo/d_ssg_sprite.c ../diablo/d_ssg_sprite.h
            ../diablo/d_progress.c    ../diablo/d_progress.h
            ../diablo/d_replay.c      ../diablo/d_replay.h
            ../diablo/d_items.c       ../diablo/d_items.h
            d_main.c        d_main.h
            d_net.c
                            doomdata.h
            doomdef.c       doomdef.h
            doomstat.c      doomstat.h
                            d_player.h
            dstrings.c      dstrings.h
                            d_textur.h
                            d_think.h
            f_finale.c      f_finale.h
            f_wipe.c        f_wipe.h
            g_game.c        g_game.h
            hu_lib.c        hu_lib.h
            hu_stuff.c      hu_stuff.h
            info.c          info.h
            m_menu.c        m_menu.h
            m_random.c      m_random.h
            p_ceilng.c
            p_doors.c
            p_enemy.c
            p_floor.c
            p_inter.c       p_inter.h
            p_lights.c
                            p_local.h
            p_map.c
            p_maputl.c
            p_mobj.c        p_mobj.h
            p_plats.c
            p_pspr.c        p_pspr.h
            p_saveg.c       p_saveg.h
            p_setup.c       p_setup.h
            p_sight.c
            p_spec.c        p_spec.h
            p_switch.c
            p_telept.c
            p_tick.c        p_tick.h
            p_user.c
            r_bsp.c         r_bsp.h
            r_data.c        r_data.h
                            r_defs.h
            r_draw.c        r_draw.h
                            r_local.h
            r_main.c        r_main.h
            r_plane.c       r_plane.h
            r_segs.c        r_segs.h
            r_sky.c         r_sky.h
                            r_state.h
            r_things.c      r_things.h
            s_sound.c       s_sound.h
            sounds.c        sounds.h
            statdump.c      statdump.h
            st_lib.c        st_lib.h
            st_stuff.c      st_stuff.h
            ../diablo/t_action.c
            ../diablo/t_combat.c      ../diablo/t_combat.h
            ../diablo/t_input.c
            ../diablo/t_turn.c        ../diablo/t_turn.h
            wi_stuff.c      wi_stuff.h)

target_include_directories(doom PRIVATE "../" "${CMAKE_CURRENT_BINARY_DIR}/../../"
                           "${CMAKE_CURRENT_SOURCE_DIR}"
                           "${CMAKE_CURRENT_SOURCE_DIR}/../diablo")
# x264 for H.264 replay encoding: prefer system via pkg-config, fall back to in-repo third_party/x264.
# If neither is available, build without replay support (NO_X264).
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(X264 QUIET x264)
endif()
set(X264_IN_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/x264")
if(X264_FOUND)
    target_include_directories(doom PRIVATE ${X264_INCLUDE_DIRS})
    set(X264_LIBS ${X264_LIBRARIES})
    message(STATUS "x264: using system library")
elseif(EXISTS "${X264_IN_REPO}/lib/libx264.a")
    target_include_directories(doom PRIVATE "${X264_IN_REPO}/include")
    set(X264_LIBS "${X264_IN_REPO}/lib/libx264.a")
    message(STATUS "x264: using in-repo third_party/x264")
else()
    message(STATUS "x264: not found - building without replay support")
    target_compile_definitions(doom PRIVATE NO_X264)
    set(X264_LIBS "")
endif()

# faac for AAC audio in replays: prefer system via pkg-config, fall back to in-repo third_party/faac.
# If neither is available, build without AAC audio (NO_FAAC) - video-only replays.
if(PKG_CONFIG_FOUND)
    pkg_check_modules(FAAC QUIET faac)
endif()
set(FAAC_IN_REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/faac")
if(FAAC_FOUND)
    target_include_directories(doom PRIVATE ${FAAC_INCLUDE_DIRS})
    set(FAAC_LIBS ${FAAC_LIBRARIES})
    message(STATUS "faac: using system library")
elseif(EXISTS "${FAAC_IN_REPO}/lib/libfaac.a")
    target_include_directories(doom PRIVATE "${FAAC_IN_REPO}/include")
    set(FAAC_LIBS "${FAAC_IN_REPO}/lib/libfaac.a")
    message(STATUS "faac: using in-repo third_party/faac")
else()
    message(STATUS "faac: not found - building without AAC audio support")
    target_compile_definitions(doom PRIVATE NO_FAAC)
    set(FAAC_LIBS "")
endif()

if (DEFINED EMSCRIPTEN)
    set(SDL_FLAGS "-s USE_SDL=2")
    if(ENABLE_SDL2_MIXER)
        set(SDL_FLAGS "${SDL_FLAGS} -s USE_SDL_MIXER=2")
    endif()
    if(ENABLE_SDL2_NET)
        set(SDL_FLAGS "${SDL_FLAGS} -s USE_SDL_NET=2")
    endif()
else()
    set(SDL_LINK_LIBS SDL2::SDL2)
    if(ENABLE_SDL2_MIXER)
        list(APPEND SDL_LINK_LIBS SDL2_mixer::SDL2_mixer)
    endif()
    if(ENABLE_SDL2_NET)
        list(APPEND SDL_LINK_LIBS SDL2_net::SDL2_net)
    endif()
endif()
target_link_libraries(doom ${SDL_LINK_LIBS} ${X264_LIBS} ${FAAC_LIBS})
set_target_properties(doom PROPERTIES COMPILE_FLAGS "${SDL_FLAGS}")
set_target_properties(doom PROPERTIES LINK_FLAGS "${SDL_FLAGS}")
