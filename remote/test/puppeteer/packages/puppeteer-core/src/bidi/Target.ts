/**
 * Copyright 2023 Google Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import type {CDPSession} from '../api/CDPSession.js';
import {Target, TargetType} from '../api/Target.js';
import {UnsupportedOperation} from '../common/Errors.js';

import type {BidiBrowser} from './Browser.js';
import type {BidiBrowserContext} from './BrowserContext.js';
import {type BrowsingContext, CdpSessionWrapper} from './BrowsingContext.js';
import {BidiPage} from './Page.js';

/**
 * @internal
 */
export abstract class BidiTarget extends Target {
  protected _browserContext: BidiBrowserContext;

  constructor(browserContext: BidiBrowserContext) {
    super();
    this._browserContext = browserContext;
  }

  _setBrowserContext(browserContext: BidiBrowserContext): void {
    this._browserContext = browserContext;
  }

  override async worker(): Promise<null> {
    return null;
  }

  override browser(): BidiBrowser {
    return this._browserContext.browser();
  }

  override browserContext(): BidiBrowserContext {
    return this._browserContext;
  }

  override opener(): never {
    throw new UnsupportedOperation();
  }

  override createCDPSession(): Promise<CDPSession> {
    throw new UnsupportedOperation();
  }
}

/**
 * @internal
 */
export class BiDiBrowserTarget extends BidiTarget {
  override url(): string {
    return '';
  }

  override type(): TargetType {
    return TargetType.BROWSER;
  }
}

/**
 * @internal
 */
export class BiDiBrowsingContextTarget extends BidiTarget {
  protected _browsingContext: BrowsingContext;

  constructor(
    browserContext: BidiBrowserContext,
    browsingContext: BrowsingContext
  ) {
    super(browserContext);

    this._browsingContext = browsingContext;
  }

  override url(): string {
    return this._browsingContext.url;
  }

  override async createCDPSession(): Promise<CDPSession> {
    const {sessionId} = await this._browsingContext.cdpSession.send(
      'Target.attachToTarget',
      {
        targetId: this._browsingContext.id,
        flatten: true,
      }
    );
    return new CdpSessionWrapper(this._browsingContext, sessionId);
  }

  override type(): TargetType {
    return TargetType.PAGE;
  }
}

/**
 * @internal
 */
export class BiDiPageTarget extends BiDiBrowsingContextTarget {
  #page: BidiPage;

  constructor(
    browserContext: BidiBrowserContext,
    browsingContext: BrowsingContext
  ) {
    super(browserContext, browsingContext);

    this.#page = new BidiPage(browsingContext, browserContext);
  }

  override async page(): Promise<BidiPage> {
    return this.#page;
  }

  override _setBrowserContext(browserContext: BidiBrowserContext): void {
    super._setBrowserContext(browserContext);
    this.#page._setBrowserContext(browserContext);
  }
}
