//! Parquet's external reader interface is implemented here, never in L1.
use asterion_kernel::files::ReadFile;
use bytes::Bytes;
use parquet::{
    errors::{ParquetError, Result},
    file::reader::{ChunkReader, Length},
};
use std::{
    io::{self, Read},
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
};

pub(crate) const TRANSFER_BYTES: usize = 64 * 1024 * 1024;
pub(crate) const FOOTER_BYTES: usize = 8 * 1024 * 1024;
#[derive(Clone)]
pub(crate) struct Source {
    pub file: ReadFile,
    pub cancelled: Arc<AtomicBool>,
}
impl Source {
    pub fn check(&self) -> crate::Result<()> {
        if self.cancelled.load(Ordering::Acquire) {
            return Err("数据扫描已关闭".into());
        }
        self.file.check().map_err(|e| e.to_string())
    }
    pub fn read_exact(&self, offset: u64, output: &mut [u8]) -> crate::Result<()> {
        self.check()?;
        let end = offset
            .checked_add(output.len() as u64)
            .ok_or("扫描读取范围溢出")?;
        if output.len() > TRANSFER_BYTES || end > self.file.len() {
            return Err("扫描读取超出文件或批量限制".into());
        }
        let mut count = 0;
        while count < output.len() {
            self.check()?;
            let got = self
                .file
                .read_at(offset + count as u64, &mut output[count..])
                .map_err(|e| e.to_string())?;
            if got == 0 {
                return Err("扫描文件意外结束".into());
            }
            count += got;
        }
        Ok(())
    }
    pub fn footer(&self) -> crate::Result<()> {
        if self.file.len() < 12 {
            return Err("Parquet 文件不完整".into());
        }
        let mut footer = [0; 8];
        self.read_exact(self.file.len() - 8, &mut footer)?;
        let length = u32::from_le_bytes(footer[..4].try_into().expect("four bytes")) as usize;
        if &footer[4..] != b"PAR1" || length > FOOTER_BYTES || length as u64 > self.file.len() - 12
        {
            return Err("Parquet 清单无效或超出读取预算".into());
        }
        Ok(())
    }
}
impl Length for Source {
    fn len(&self) -> u64 {
        self.file.len()
    }
}
pub(crate) struct Reader {
    source: Source,
    offset: u64,
}
impl Read for Reader {
    fn read(&mut self, output: &mut [u8]) -> io::Result<usize> {
        self.source.check().map_err(io::Error::other)?;
        let size = output.len().min(TRANSFER_BYTES);
        let count = self
            .source
            .file
            .read_at(self.offset, &mut output[..size])
            .map_err(io::Error::other)?;
        self.offset += count as u64;
        Ok(count)
    }
}
impl ChunkReader for Source {
    type T = Reader;
    fn get_read(&self, start: u64) -> Result<Reader> {
        self.check().map_err(ParquetError::General)?;
        if start > self.len() {
            return Err(ParquetError::General("扫描读取超出文件".into()));
        }
        Ok(Reader {
            source: self.clone(),
            offset: start,
        })
    }
    fn get_bytes(&self, start: u64, length: usize) -> Result<Bytes> {
        if length > TRANSFER_BYTES {
            return Err(ParquetError::General("Parquet 读取块超出预算".into()));
        }
        let mut output = vec![0; length];
        self.read_exact(start, &mut output)
            .map_err(ParquetError::General)?;
        Ok(output.into())
    }
}
