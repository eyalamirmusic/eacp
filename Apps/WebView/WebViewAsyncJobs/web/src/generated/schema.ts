export interface SearchRequest {
    query: string;
}

export interface SearchHit {
    title: string;
    score: number;
}

export interface SearchResults {
    query: string;
    hits: SearchHit[];
}

