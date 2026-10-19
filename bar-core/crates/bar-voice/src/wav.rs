pub fn pcm16_to_wav(pcm: &[u8], sample_rate: u32) -> Vec<u8> {
    let channels = 1u16;
    let bits_per_sample = 16u16;
    let byte_rate = sample_rate * u32::from(channels) * u32::from(bits_per_sample) / 8;
    let block_align = channels * bits_per_sample / 8;
    let data_len = pcm.len() as u32;
    let mut out = Vec::with_capacity(44 + pcm.len());
    out.extend_from_slice(b"RIFF");
    out.extend_from_slice(&(36 + data_len).to_le_bytes());
    out.extend_from_slice(b"WAVE");
    out.extend_from_slice(b"fmt ");
    out.extend_from_slice(&16u32.to_le_bytes());
    out.extend_from_slice(&1u16.to_le_bytes());
    out.extend_from_slice(&channels.to_le_bytes());
    out.extend_from_slice(&sample_rate.to_le_bytes());
    out.extend_from_slice(&byte_rate.to_le_bytes());
    out.extend_from_slice(&block_align.to_le_bytes());
    out.extend_from_slice(&bits_per_sample.to_le_bytes());
    out.extend_from_slice(b"data");
    out.extend_from_slice(&data_len.to_le_bytes());
    out.extend_from_slice(pcm);
    out
}

/// Downsample / convert float WAV-ish PCM from Piper (often 22050) to 16k i16 LE.
pub fn resample_i16_to_16k(pcm: &[i16], src_rate: u32) -> Vec<u8> {
    if src_rate == 16000 {
        return pcm
            .iter()
            .flat_map(|s| s.to_le_bytes())
            .collect();
    }
    let ratio = src_rate as f64 / 16000.0;
    let out_len = ((pcm.len() as f64) / ratio) as usize;
    let mut out = Vec::with_capacity(out_len * 2);
    for i in 0..out_len {
        let src_idx = (i as f64 * ratio) as usize;
        let s = pcm.get(src_idx).copied().unwrap_or(0);
        out.extend_from_slice(&s.to_le_bytes());
    }
    out
}
