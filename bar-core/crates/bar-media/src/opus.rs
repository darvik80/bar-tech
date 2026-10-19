use anyhow::{anyhow, Result};
use libopus_sys::*;
use std::ptr;

const SAMPLE_RATE: i32 = 16_000;
const CHANNELS: i32 = 1;
const FRAME_SAMPLES: usize = 960; // 60 ms @ 16 kHz
const MAX_PACKET: usize = 4000;

pub struct OpusCodec {
    encoder: *mut OpusEncoder,
    decoder: *mut OpusDecoder,
}

unsafe impl Send for OpusCodec {}

impl OpusCodec {
    pub fn new() -> Result<Self> {
        unsafe {
            let mut err = 0;
            let encoder = opus_encoder_create(
                SAMPLE_RATE,
                CHANNELS,
                OPUS_APPLICATION_AUDIO as i32,
                &mut err,
            );
            if err != OPUS_OK as i32 || encoder.is_null() {
                return Err(anyhow!("opus_encoder_create failed: {err}"));
            }
            let decoder = opus_decoder_create(SAMPLE_RATE, CHANNELS, &mut err);
            if err != OPUS_OK as i32 || decoder.is_null() {
                opus_encoder_destroy(encoder);
                return Err(anyhow!("opus_decoder_create failed: {err}"));
            }
            Ok(Self { encoder, decoder })
        }
    }

    pub fn frame_samples() -> usize {
        FRAME_SAMPLES
    }

    pub fn sample_rate() -> u32 {
        SAMPLE_RATE as u32
    }

    pub fn decode(&mut self, packet: &[u8]) -> Result<Vec<i16>> {
        let mut pcm = vec![0i16; FRAME_SAMPLES * 2];
        let n = unsafe {
            opus_decode(
                self.decoder,
                packet.as_ptr(),
                packet.len() as i32,
                pcm.as_mut_ptr(),
                FRAME_SAMPLES as i32,
                0,
            )
        };
        if n < 0 {
            return Err(anyhow!("opus_decode failed: {n}"));
        }
        pcm.truncate(n as usize);
        Ok(pcm)
    }

    pub fn encode(&mut self, pcm: &[i16]) -> Result<Vec<u8>> {
        let mut frame = vec![0i16; FRAME_SAMPLES];
        let copy = pcm.len().min(FRAME_SAMPLES);
        frame[..copy].copy_from_slice(&pcm[..copy]);
        let mut out = vec![0u8; MAX_PACKET];
        let n = unsafe {
            opus_encode(
                self.encoder,
                frame.as_ptr(),
                FRAME_SAMPLES as i32,
                out.as_mut_ptr(),
                MAX_PACKET as i32,
            )
        };
        if n < 0 {
            return Err(anyhow!("opus_encode failed: {n}"));
        }
        out.truncate(n as usize);
        Ok(out)
    }

    pub fn encode_pcm_bytes(&mut self, pcm: &[u8]) -> Result<Vec<Vec<u8>>> {
        let samples: Vec<i16> = pcm
            .chunks_exact(2)
            .map(|c| i16::from_le_bytes([c[0], c[1]]))
            .collect();
        let mut frames = Vec::new();
        for chunk in samples.chunks(FRAME_SAMPLES) {
            frames.push(self.encode(chunk)?);
        }
        if frames.is_empty() {
            frames.push(self.encode(&[])?);
        }
        Ok(frames)
    }
}

impl Drop for OpusCodec {
    fn drop(&mut self) {
        unsafe {
            if !self.encoder.is_null() {
                opus_encoder_destroy(self.encoder);
                self.encoder = ptr::null_mut();
            }
            if !self.decoder.is_null() {
                opus_decoder_destroy(self.decoder);
                self.decoder = ptr::null_mut();
            }
        }
    }
}
