export interface SearchDownloadsRequest {
    query: string;
}

export interface DownloadResult {
    name: string;
    path: string;
    kind: string;
    size: number;
    score: number;
}

export interface SearchDownloadsResponse {
    results: DownloadResult[];
}

export interface ArmDragRequest {
    paths: string[];
}

export interface CopyFilesRequest {
    paths: string[];
}

export interface CopyFilesResponse {
    copied: boolean;
}

export interface PlayAudioRequest {
    path: string;
}

export interface PlaybackState {
    playing: boolean;
    path: string;
}

export interface SubmitPromptRequest {
    text: string;
}

