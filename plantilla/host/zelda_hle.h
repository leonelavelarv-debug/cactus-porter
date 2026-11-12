// SPDX-License-Identifier: GPL-2.0-or-later
// zelda_hle.h — HLE del microcódigo "Zelda" del DSP (Super Mario Galaxy 1 y 2, Zelda TP, Pikmin NPC, y los de
// GameCube: Wind Waker, Sunshine, Luigi's Mansion...). Adaptado de Dolphin
// (Source/Core/Core/HW/DSPHLE/UCodes/Zelda.cpp, Copyright 2008 Dolphin Emulator Project, GPL-2.0-or-later):
// mismo protocolo de correos, mismos comandos y el mismo mezclador (VPB, AFC/PCM, resampleo, reverb, Dolby).
// No depende de SDL ni de DolRecomp: dsp_hle.cpp le pasa la memoria y recoge sus correos.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace zelda {

struct Env {
    uint8_t* (*span)(uint32_t addr, size_t len);          // memoria principal (MEM1/MEM2)
    void (*mem_written)(uint32_t addr, size_t len);       // opcional: avisa de escrituras del DSP
    uint8_t* (*aram)(uint32_t offset, size_t len);        // ARAM de GameCube (los de Wii leen de memoria)
    void (*push_mail)(uint32_t mail, bool irq);           // correo DSP -> CPU
    void (*raise_dspint)();                               // interrupción sin correo (protocolo "light")
};

// Lo que el microcódigo pide al DSP tras un correo.
enum class Action { None, UploadNewUcode, ResetToRom };

// Banderas por versión (las de Dolphin). 0 si el hash no es de la familia Zelda.
bool known(uint32_t ector_hash);
uint32_t flags_for(uint32_t ector_hash);

class Renderer;

class Ucode {
 public:
    Ucode(const Env& env, uint32_t ector_hash);
    ~Ucode();
    void initialize();                 // correos de arranque (DSP_INIT + 0xF3551111, o 0x88881111)
    Action handle_mail(uint32_t mail);
    uint32_t hash() const { return hash_; }
    uint32_t flags() const { return flags_; }
    uint32_t frames_rendered() const { return frames_rendered_; }
    bool halted() const { return state_ == State::Halted; }

 private:
    enum class State { Waiting, Rendering, WritingCmd, Halted };
    enum class Ack { Standard, DoneRendering };

    Action mail_default(uint32_t mail);
    Action mail_light(uint32_t mail);
    uint32_t read32();
    void write32(uint32_t v);
    void run_pending_commands();
    void send_ack(Ack type, uint16_t sync);
    bool rendering() const { return cur_frame_ != req_frames_; }
    void render_audio();

    Env env_;
    uint32_t hash_, flags_;
    State state_ = State::Waiting;
    uint32_t expected_cmd_mails_ = 0;
    uint32_t sync_max_voice_ = 0;
    std::array<uint16_t, 256> sync_skip_{};
    bool sync_second_half_ = false;
    std::array<uint32_t, 64> cmd_{};
    uint32_t rd_ = 0, wr_ = 0, pending_ = 0;
    bool can_execute_ = true;
    uint32_t req_frames_ = 0, cur_frame_ = 0, cur_voice_ = 0;
    uint16_t voices_per_frame_ = 0;
    uint32_t frames_rendered_ = 0;
    std::unique_ptr<Renderer> r_;
};

}  // namespace zelda
