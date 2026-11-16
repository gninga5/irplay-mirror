//! Ricerca su byte, con la semantica di std::string_view::find (posizione oltre la fine:
//! nessun risultato). I parser lavorano su byte e non su str: i dati di rete non sono
//! necessariamente UTF-8 e devono passare intatti.

/// Prima occorrenza di `needle` in `hay` a partire da `from`.
pub fn find(hay: &[u8], needle: &[u8], from: usize) -> Option<usize> {
    if from > hay.len() || needle.len() > hay.len() - from {
        return None;
    }
    if needle.is_empty() {
        return Some(from);
    }
    let first = needle[0];
    let last_start = hay.len() - needle.len();
    let mut i = from;
    while i <= last_start {
        i += hay[i..=last_start].iter().position(|&b| b == first)?;
        if &hay[i..i + needle.len()] == needle {
            return Some(i);
        }
        i += 1;
    }
    None
}

/// Prima occorrenza del byte `b` a partire da `from`.
pub fn find_byte(hay: &[u8], b: u8, from: usize) -> Option<usize> {
    if from > hay.len() {
        return None;
    }
    hay[from..].iter().position(|&c| c == b).map(|p| p + from)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn find_like_string_view() {
        assert_eq!(find(b"abcabc", b"bc", 0), Some(1));
        assert_eq!(find(b"abcabc", b"bc", 2), Some(4));
        assert_eq!(find(b"abc", b"", 3), Some(3));
        assert_eq!(find(b"abc", b"", 4), None);
        assert_eq!(find(b"abc", b"abcd", 0), None);
        assert_eq!(find(b"aab", b"ab", 0), Some(1));
        assert_eq!(find_byte(b"a,b", b',', 0), Some(1));
        assert_eq!(find_byte(b"a,b", b',', 3), None);
        assert_eq!(find_byte(b"a,b", b',', 4), None);
    }
}
